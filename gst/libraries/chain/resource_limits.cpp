#include <gstio/chain/exceptions.hpp>
#include <gstio/chain/resource_limits.hpp>
#include <gstio/chain/resource_limits_private.hpp>
#include <gstio/chain/transaction_metadata.hpp>
#include <gstio/chain/transaction.hpp>
#include <boost/tuple/tuple_io.hpp>
#include <gstio/chain/database_utils.hpp>
#include <algorithm>

namespace gstio { namespace chain { namespace resource_limits {

using resource_index_set = index_set<
   resource_limits_index,
   resource_usage_index,
   resource_gst_index,
   resource_activation_gst_index,
   resource_limits_state_index,
   resource_limits_config_index
>;

static_assert( config::rate_limiting_precision > 0, "config::rate_limiting_precision must be positive" );

static uint64_t update_elastic_limit(uint64_t current_limit, uint64_t average_usage, const elastic_limit_parameters& params) {
   uint64_t result = current_limit;
   if (average_usage > params.target ) {
      result = result * params.contract_rate;
   } else {
      result = result * params.expand_rate;
   }
   return std::min(std::max(result, params.max), params.max * params.max_multiplier);
}

void elastic_limit_parameters::validate()const {
   // At the very least ensure parameters are not set to values that will cause divide by zero errors later on.
   // Stricter checks for sensible values can be added later.
   GST_ASSERT( periods > 0, resource_limit_exception, "elastic limit parameter 'periods' cannot be zero" );
   GST_ASSERT( contract_rate.denominator > 0, resource_limit_exception, "elastic limit parameter 'contract_rate' is not a well-defined ratio" );
   GST_ASSERT( expand_rate.denominator > 0, resource_limit_exception, "elastic limit parameter 'expand_rate' is not a well-defined ratio" );
}


void resource_limits_state_object::update_virtual_cpu_limit( const resource_limits_config_object& cfg ) {
   //idump((average_block_cpu_usage.average()));
   virtual_cpu_limit = update_elastic_limit(virtual_cpu_limit, average_block_cpu_usage.average(), cfg.cpu_limit_parameters);
   //idump((virtual_cpu_limit));
}

void resource_limits_state_object::update_virtual_net_limit( const resource_limits_config_object& cfg ) {
   virtual_net_limit = update_elastic_limit(virtual_net_limit, average_block_net_usage.average(), cfg.net_limit_parameters);
}

void resource_limits_manager::add_indices() {
   resource_index_set::add_indices(_db);
}

void resource_limits_manager::initialize_database() {
   const auto& config = _db.create<resource_limits_config_object>([](resource_limits_config_object& config){
      // see default settings in the declaration
   });

   _db.create<resource_limits_state_object>([&config](resource_limits_state_object& state){
      // see default settings in the declaration

      // start the chain off in a way that it is "congested" aka slow-start
      state.virtual_cpu_limit = config.cpu_limit_parameters.max;
      state.virtual_net_limit = config.net_limit_parameters.max;
   });
}

void resource_limits_manager::add_to_snapshot( const snapshot_writer_ptr& snapshot ) const {
   resource_index_set::walk_indices([this, &snapshot]( auto utils ){
      snapshot->write_section<typename decltype(utils)::index_t::value_type>([this]( auto& section ){
         decltype(utils)::walk(_db, [this, &section]( const auto &row ) {
            section.add_row(row, _db);
         });
      });
   });
}

void resource_limits_manager::read_from_snapshot( const snapshot_reader_ptr& snapshot ) {
   resource_index_set::walk_indices([this, &snapshot]( auto utils ){
      snapshot->read_section<typename decltype(utils)::index_t::value_type>([this]( auto& section ) {
         bool more = !section.empty();
         while(more) {
            decltype(utils)::create(_db, [this, &section, &more]( auto &row ) {
               more = section.read_row(row, _db);
            });
         }
      });
   });
}

void resource_limits_manager::initialize_account(const account_name& account) {
   _db.create<resource_limits_object>([&]( resource_limits_object& bl ) {
      bl.owner = account;
   });

   _db.create<resource_usage_object>([&]( resource_usage_object& bu ) {
      bu.owner = account;
   });
    //std::cout<<"D__新增的用户:"<<account<<std::endl;
   // _db.create<resource_gst_object>([&]( resource_gst_object& bg ) {   //新增的表
   //     bg.owner = account;
   //  });
}

void resource_limits_manager::set_block_parameters(const elastic_limit_parameters& cpu_limit_parameters, const elastic_limit_parameters& net_limit_parameters ) {
   cpu_limit_parameters.validate();
   net_limit_parameters.validate();
   const auto& config = _db.get<resource_limits_config_object>();
   _db.modify(config, [&](resource_limits_config_object& c){
      c.cpu_limit_parameters = cpu_limit_parameters;
      c.net_limit_parameters = net_limit_parameters;
   });
}

void resource_limits_manager::update_account_usage(const flat_set<account_name>& accounts, uint32_t time_slot ) {
   const auto& config = _db.get<resource_limits_config_object>();
   for( const auto& a : accounts ) {
      const auto& usage = _db.get<resource_usage_object,by_owner>( a );
      _db.modify( usage, [&]( auto& bu ){
          bu.net_usage.add( 0, time_slot, config.account_net_usage_average_window );
          bu.cpu_usage.add( 0, time_slot, config.account_cpu_usage_average_window );
      });
   }
}

void resource_limits_manager::add_transaction_usage(const flat_set<account_name>& accounts, uint64_t cpu_usage, uint64_t net_usage, uint32_t time_slot ) {
   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();

   for( const auto& a : accounts ) {

      const auto& usage = _db.get<resource_usage_object,by_owner>( a );
      int64_t unused;
      int64_t net_weight;
      int64_t cpu_weight;
      get_account_limits( a, unused, net_weight, cpu_weight );

      _db.modify( usage, [&]( auto& bu ){
          bu.net_usage.add( net_usage, time_slot, config.account_net_usage_average_window );
          bu.cpu_usage.add( cpu_usage, time_slot, config.account_cpu_usage_average_window );
      });

      if( cpu_weight >= 0 && state.total_cpu_weight > 0 ) {
         uint128_t window_size = config.account_cpu_usage_average_window;
         auto virtual_network_capacity_in_window = (uint128_t)state.virtual_cpu_limit * window_size;
         auto cpu_used_in_window                 = ((uint128_t)usage.cpu_usage.value_ex * window_size) / (uint128_t)config::rate_limiting_precision;

         uint128_t user_weight     = (uint128_t)cpu_weight;
         uint128_t all_user_weight = state.total_cpu_weight;

         auto max_user_use_in_window = (virtual_network_capacity_in_window * user_weight) / all_user_weight;

         GST_ASSERT( cpu_used_in_window <= max_user_use_in_window,
                     tx_cpu_usage_exceeded,
                     "authorizing account '${n}' has insufficient cpu resources for this transaction",
                     ("n", name(a))
                     ("cpu_used_in_window",cpu_used_in_window)
                     ("max_user_use_in_window",max_user_use_in_window) );
      }

      if( net_weight >= 0 && state.total_net_weight > 0) {

         uint128_t window_size = config.account_net_usage_average_window;
         auto virtual_network_capacity_in_window = (uint128_t)state.virtual_net_limit * window_size;
         auto net_used_in_window                 = ((uint128_t)usage.net_usage.value_ex * window_size) / (uint128_t)config::rate_limiting_precision;

         uint128_t user_weight     = (uint128_t)net_weight;
         uint128_t all_user_weight = state.total_net_weight;

         auto max_user_use_in_window = (virtual_network_capacity_in_window * user_weight) / all_user_weight;

         GST_ASSERT( net_used_in_window <= max_user_use_in_window,
                     tx_net_usage_exceeded,
                     "authorizing account '${n}' has insufficient net resources for this transaction",
                     ("n", name(a))
                     ("net_used_in_window",net_used_in_window)
                     ("max_user_use_in_window",max_user_use_in_window) );

      }
   }

   // account for this transaction in the block and do not exceed those limits either
   _db.modify(state, [&](resource_limits_state_object& rls){
      rls.pending_cpu_usage += cpu_usage;
      rls.pending_net_usage += net_usage;
   });

   GST_ASSERT( state.pending_cpu_usage <= config.cpu_limit_parameters.max, block_resource_exhausted, "Block has insufficient cpu resources" );
   GST_ASSERT( state.pending_net_usage <= config.net_limit_parameters.max, block_resource_exhausted, "Block has insufficient net resources" );
}

void resource_limits_manager::add_pending_ram_usage( const account_name account, int64_t ram_delta ) {
   if (ram_delta == 0) {
      return;
   }

   const auto& usage  = _db.get<resource_usage_object,by_owner>( account );

   GST_ASSERT( ram_delta <= 0 || UINT64_MAX - usage.ram_usage >= (uint64_t)ram_delta, transaction_exception,
              "Ram usage delta would overflow UINT64_MAX");
   GST_ASSERT(ram_delta >= 0 || usage.ram_usage >= (uint64_t)(-ram_delta), transaction_exception,
              "Ram usage delta would underflow UINT64_MAX");

   _db.modify( usage, [&]( auto& u ) {
     u.ram_usage += ram_delta;
   });

   // std::cout<<"D__当前用户"<<account<<"消耗的gstbyte: "<<ram_delta<<std::endl;
   if(is_activation()){
      const auto* pending_limits = _db.find<resource_gst_object, by_owner>( boost::make_tuple(true, account) );
      if(pending_limits != nullptr){    //系统第一次部署合约会走这儿
         //更新消耗的表
         auto& limits1 = * pending_limits;
         _db.modify( limits1, [&]( resource_gst_object& limits ){
            if((int64_t)(limits.gst_usage) + ram_delta < 0){ //防止老用户部署的新合约小于旧合约，消耗溢出成无限大
               limits.gst_usage = 0;
            }else{
               limits.gst_usage += ram_delta;
            }
            //std::cout<<"D__当前用户"<<limits.owner<<" 消耗了 " <<limits.gst_usage<<std::endl;
         });
      }else{  
         //老用户第一次进来时先为他们创表                                                            );  
            _db.create<resource_gst_object>([&](resource_gst_object& pending_limits){
                     pending_limits.owner = account;
                     pending_limits.gst_bytes = 0;
                     pending_limits.pending = true;
                     if(ram_delta < 0){
                        pending_limits.gst_usage = 0;
                     }else
                     {
                        pending_limits.gst_usage = ram_delta;
                     }
            });
          //  }
      }
   }

}

void resource_limits_manager::verify_account_ram_usage( const account_name account )const {

   int64_t ram_bytes; int64_t net_weight; int64_t cpu_weight;
   get_account_limits( account, ram_bytes, net_weight, cpu_weight );
   const auto& usage  = _db.get<resource_usage_object,by_owner>( account );

   if( ram_bytes >= 0 ) {
      GST_ASSERT( usage.ram_usage <= static_cast<uint64_t>(ram_bytes), ram_usage_exceeded,
                  "account ${account} has insufficient ram; needs ${needs} bytes has ${available} bytes",
                  ("account", account)("needs",usage.ram_usage)("available",ram_bytes)              );
   }

   if(is_activation()){
      const auto* pending_limits = _db.find<resource_gst_object, by_owner>( boost::make_tuple(true, account) );
      if(pending_limits != nullptr){  //系统第一次部署合约会走这儿
         if(pending_limits->gst_bytes >= 0 && pending_limits->owner != "gstio.gas" && pending_limits->owner != "gstio"){ //gstio.gas用户不计算gas消耗
            GST_ASSERT( pending_limits->gst_usage <= static_cast<uint64_t>(pending_limits->gst_bytes), gstio_assert_message_exception,
                        "account ${account} has insufficient gas; needs ${needs} gas has ${available} gas",
                        ("account", account)("needs",pending_limits->gst_usage)("available",pending_limits->gst_bytes)              );
         }
      }else{   // gstio需要部署系统合约，当一次创建gstgas时,操作也不耗费手续费
            GST_ASSERT( ( "gstio.gas" == account),gstio_assert_message_exception,"用户${account}请兑换gas再进行此操作",
                        ("account", account)                                                          );
      
      }
   }
}

void resource_limits_manager::verify_account_gst_usage( const name account )const {
   int64_t gst_bytes;
   const auto* pending_limits = _db.find<resource_gst_object, by_owner>( boost::make_tuple(true, account) );
   //老版本的账户进来时要把兑换的gst资源重新写表
   GST_ASSERT( pending_limits != nullptr,gstio_assert_message_exception,"用户${account}请兑换gas再进行此操作",
                   ("account", account)                                                                  );
   gst_bytes = pending_limits->gst_bytes-pending_limits->gst_usage;
   //std::cout<<"D__当前用户"<<account<<" gst_bytes" <<pending_limits->gst_bytes<<std::endl;
   if(pending_limits->gst_bytes >=0 ){
      GST_ASSERT( pending_limits->gst_bytes  >= pending_limits->gst_usage+100,gstio_assert_message_exception,
                  "用户 ${account} gas不足; 需要 ${needs} gas 剩余 ${available} gas",
                     ("account", account)("needs",100)("available",gst_bytes)              );
      //更新消耗的表
      auto& limits1 = * pending_limits;
      _db.modify( limits1, [&]( resource_gst_object& limits ){
         limits.gst_usage += 100;
         //std::cout<<"D__当前用户"<<limits.owner<<" 消耗了" <<limits.gst_usage<<std::endl;
      });
   }
}

bool resource_limits_manager::is_activation()const{
   //std::cout<<"D__进入is_activation函数"<<std::endl;
   name acname("gstio");
   const auto*  activation_gst = _db.find<resource_activation_gst_object, by_owner>( boost::make_tuple(true, acname) );
   //std::cout<<"D__查看gst gas是否激活"<<std::endl;
   if(activation_gst == nullptr){
      return false;
   }
   if(!activation_gst->is_activation){
      //std::cout<<"D__查看gst gas 目前已停止使用"<<std::endl;
      return false;
   }
   //std::cout<<"D__查看gst gas已激活"<<std::endl;
   return true;
}

int64_t resource_limits_manager::get_account_ram_usage( const account_name& name )const {
   return _db.get<resource_usage_object,by_owner>( name ).ram_usage;
}


bool resource_limits_manager::set_account_limits( const account_name& account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight) {
   //const auto& usage = _db.get<resource_usage_object,by_owner>( account );
   /*
    * Since we need to delay these until the next resource limiting boundary, these are created in a "pending"
    * state or adjusted in an existing "pending" state.  The chain controller will collapse "pending" state into
    * the actual state at the next appropriate boundary.
    */
   auto find_or_create_pending_limits = [&]() -> const resource_limits_object& {
      const auto* pending_limits = _db.find<resource_limits_object, by_owner>( boost::make_tuple(true, account) );
      if (pending_limits == nullptr) {
         const auto& limits = _db.get<resource_limits_object, by_owner>( boost::make_tuple(false, account));
         return _db.create<resource_limits_object>([&](resource_limits_object& pending_limits){
            pending_limits.owner = limits.owner;
            pending_limits.ram_bytes = limits.ram_bytes;
            pending_limits.net_weight = limits.net_weight;
            pending_limits.cpu_weight = limits.cpu_weight;
            pending_limits.pending = true;
         });
      } else {
         return *pending_limits;
      }
   };

   // update the users weights directly
   auto& limits = find_or_create_pending_limits();

   bool decreased_limit = false;

   if( ram_bytes >= 0 ) {

      decreased_limit = ( (limits.ram_bytes < 0) || (ram_bytes < limits.ram_bytes) );

      /*
      if( limits.ram_bytes < 0 ) {
         GST_ASSERT(ram_bytes >= usage.ram_usage, wasm_execution_error, "converting unlimited account would result in overcommitment [commit=${c}, desired limit=${l}]", ("c", usage.ram_usage)("l", ram_bytes));
      } else {
         GST_ASSERT(ram_bytes >= usage.ram_usage, wasm_execution_error, "attempting to release committed ram resources [commit=${c}, desired limit=${l}]", ("c", usage.ram_usage)("l", ram_bytes));
      }
      */
   }

   _db.modify( limits, [&]( resource_limits_object& pending_limits ){
      pending_limits.ram_bytes = ram_bytes;
      pending_limits.net_weight = net_weight;
      pending_limits.cpu_weight = cpu_weight;
   });

   return decreased_limit;
}

bool resource_limits_manager::set_gst_limits( const account_name& account, int64_t gst_bytes) {
  
   auto find_or_create_pending_limits = [&]() -> const resource_gst_object& {
      const auto* pending_limits = _db.find<resource_gst_object, by_owner>( boost::make_tuple(true, account) );
      if (pending_limits == nullptr) {
         //std::cout<<"D__为新用户设置"<<account<<"  "<<gst_bytes<<std::endl;
         // const auto& limits = _db.get<resource_gst_object, by_owner>( boost::make_tuple(false, account));
         // if(limits==nullptr){   //后期加的表，有可能数据库里没先前的数据,不采用这种方法

         //    return _db.create<resource_gst_object>([&](resource_gst_object& pending_limits){
         //    pending_limits.owner = account;
         //    pending_limits.gst_bytes = gst_bytes;
         //    pending_limits.pending = true;
         //    }
         // }else{
            return _db.create<resource_gst_object>([&](resource_gst_object& pending_limits){
               pending_limits.owner = account;
               pending_limits.gst_bytes = gst_bytes;
               pending_limits.pending = true;
            });
         //}
      } else {
         return *pending_limits;
      }
   };

   // update the users weights directly
   auto& limits = find_or_create_pending_limits();

   if( limits.gst_bytes > gst_bytes ){      //出售了资源
      GST_ASSERT( gst_bytes  >= limits.gst_usage,gstio_assert_message_exception,
                  "用户 ${account} gas不足; 当前剩余 ${needs} gas 已用 ${available} gas",
                     ("account", account)("needs",(limits.gst_bytes-limits.gst_usage))("available",limits.gst_usage)              );
   }


   bool decreased_limit = false;

   if( gst_bytes >= 0 ) {

      decreased_limit = ( (limits.gst_bytes < 0) || (gst_bytes < limits.gst_bytes) );

   }
   //std::cout<<"D__当前用户"<<account<<"   gst_bytes总量为"<<gst_bytes<<std::endl;
   _db.modify( limits, [&]( resource_gst_object& pending_limits ){
      pending_limits.gst_bytes = gst_bytes;

   });

   // 取消这种流程，改为开关激活
   // //第一次购买资源时激活resource_activation_gst_object
   // name acname("gstio");
   // const auto*  activation_gst = _db.find<resource_activation_gst_object, by_owner>( boost::make_tuple(true, acname) );
   // std::cout<<"D__gst gas已经激活"<<std::endl;
   // if(activation_gst == nullptr){
   //     std::cout<<"D__gst gas已经激活2222"<<std::endl;
   //    _db.create<resource_activation_gst_object>([&](resource_activation_gst_object& activation_gst){
   //       activation_gst.owner = acname;
   //       activation_gst.is_activation = true;
   //       activation_gst.value = 1;
   //     });
   // }

   return decreased_limit;
}

void resource_limits_manager::set_gas_limits(bool flag){
   name acname("gstio");
   std::cout<<"D__是否激活： "<<flag<<std::endl;
   const auto*  activation_gst = _db.find<resource_activation_gst_object, by_owner>( boost::make_tuple(true, acname) );
   if(activation_gst == nullptr){
      std::cout<<"D__第一次创建激活账户"<<std::endl;
      _db.create<resource_activation_gst_object>([&](resource_activation_gst_object& activation_gst){
         activation_gst.owner = acname;
         activation_gst.pending = true;
         activation_gst.is_activation = true;
       });
   }else
   {
      std::cout<<"D__修改激活状态"<<std::endl;
      auto & gas = *activation_gst;
      _db.modify( gas, [&]( resource_activation_gst_object& gas ){
         gas.is_activation = flag;
      });
   }
   
}

void resource_limits_manager::get_account_limits( const account_name& account, int64_t& ram_bytes, int64_t& net_weight, int64_t& cpu_weight ) const {
   const auto* pending_buo = _db.find<resource_limits_object,by_owner>( boost::make_tuple(true, account) );
   if (pending_buo) {
      ram_bytes  = pending_buo->ram_bytes;
      net_weight = pending_buo->net_weight;
      cpu_weight = pending_buo->cpu_weight;
   } else {
      const auto& buo = _db.get<resource_limits_object,by_owner>( boost::make_tuple( false, account ) );
      ram_bytes  = buo.ram_bytes;
      net_weight = buo.net_weight;
      cpu_weight = buo.cpu_weight;
   }
   
}

void resource_limits_manager::get_account_limits2( const account_name& account, int64_t& ram_bytes)const{
   const auto* pending_limits = _db.find<resource_gst_object, by_owner>( boost::make_tuple(true, account) );
   if(pending_limits != nullptr){   
      //查询剩余的gas
      auto& limits1 = * pending_limits;
      ram_bytes = limits1.gst_bytes - limits1.gst_usage;
      if(ram_bytes < 0){
         ram_bytes = 0;
      }
   }else{
      ram_bytes = 0;
   }
}


void resource_limits_manager::process_account_limit_updates() {
   auto& multi_index = _db.get_mutable_index<resource_limits_index>();
   auto& by_owner_index = multi_index.indices().get<by_owner>();

   // convenience local lambda to reduce clutter
   auto update_state_and_value = [](uint64_t &total, int64_t &value, int64_t pending_value, const char* debug_which) -> void {
      if (value > 0) {
         GST_ASSERT(total >= static_cast<uint64_t>(value), rate_limiting_state_inconsistent, "underflow when reverting old value to ${which}", ("which", debug_which));
         total -= value;
      }

      if (pending_value > 0) {
         GST_ASSERT(UINT64_MAX - total >= static_cast<uint64_t>(pending_value), rate_limiting_state_inconsistent, "overflow when applying new value to ${which}", ("which", debug_which));
         total += pending_value;
      }

      value = pending_value;
   };

   const auto& state = _db.get<resource_limits_state_object>();
   _db.modify(state, [&](resource_limits_state_object& rso){
      while(!by_owner_index.empty()) {
         const auto& itr = by_owner_index.lower_bound(boost::make_tuple(true));
         if (itr == by_owner_index.end() || itr->pending!= true) {
            break;
         }

         const auto& actual_entry = _db.get<resource_limits_object, by_owner>(boost::make_tuple(false, itr->owner));
         _db.modify(actual_entry, [&](resource_limits_object& rlo){
            update_state_and_value(rso.total_ram_bytes,  rlo.ram_bytes,  itr->ram_bytes, "ram_bytes");
            update_state_and_value(rso.total_cpu_weight, rlo.cpu_weight, itr->cpu_weight, "cpu_weight");
            update_state_and_value(rso.total_net_weight, rlo.net_weight, itr->net_weight, "net_weight");
         });

         multi_index.remove(*itr);
      }
   });
}

void resource_limits_manager::process_block_usage(uint32_t block_num) {
   const auto& s = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   _db.modify(s, [&](resource_limits_state_object& state){
      // apply pending usage, update virtual limits and reset the pending

      state.average_block_cpu_usage.add(state.pending_cpu_usage, block_num, config.cpu_limit_parameters.periods);
      state.update_virtual_cpu_limit(config);
      state.pending_cpu_usage = 0;

      state.average_block_net_usage.add(state.pending_net_usage, block_num, config.net_limit_parameters.periods);
      state.update_virtual_net_limit(config);
      state.pending_net_usage = 0;

   });

}

uint64_t resource_limits_manager::get_virtual_block_cpu_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.virtual_cpu_limit;
}

uint64_t resource_limits_manager::get_virtual_block_net_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.virtual_net_limit;
}

uint64_t resource_limits_manager::get_block_cpu_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   return config.cpu_limit_parameters.max - state.pending_cpu_usage;
}

uint64_t resource_limits_manager::get_block_net_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   return config.net_limit_parameters.max - state.pending_net_usage;
}

int64_t resource_limits_manager::get_account_cpu_limit( const account_name& name, bool elastic ) const {
   auto arl = get_account_cpu_limit_ex(name, elastic);
   return arl.available;
}

account_resource_limit resource_limits_manager::get_account_cpu_limit_ex( const account_name& name, bool elastic) const {

   const auto& state = _db.get<resource_limits_state_object>();
   const auto& usage = _db.get<resource_usage_object, by_owner>(name);
   const auto& config = _db.get<resource_limits_config_object>();

   int64_t cpu_weight, x, y;
   get_account_limits( name, x, y, cpu_weight );

   if( cpu_weight < 0 || state.total_cpu_weight == 0 ) {
      return { -1, -1, -1 };
   }

   account_resource_limit arl;

   uint128_t window_size = config.account_cpu_usage_average_window;

   uint128_t virtual_cpu_capacity_in_window = (uint128_t)(elastic ? state.virtual_cpu_limit : config.cpu_limit_parameters.max) * window_size;
   uint128_t user_weight     = (uint128_t)cpu_weight;
   uint128_t all_user_weight = (uint128_t)state.total_cpu_weight;

   auto max_user_use_in_window = (virtual_cpu_capacity_in_window * user_weight) / all_user_weight;
   auto cpu_used_in_window  = impl::integer_divide_ceil((uint128_t)usage.cpu_usage.value_ex * window_size, (uint128_t)config::rate_limiting_precision);

   if( max_user_use_in_window <= cpu_used_in_window )
      arl.available = 0;
   else
      arl.available = impl::downgrade_cast<int64_t>(max_user_use_in_window - cpu_used_in_window);

   arl.used = impl::downgrade_cast<int64_t>(cpu_used_in_window);
   arl.max = impl::downgrade_cast<int64_t>(max_user_use_in_window);
   return arl;
}

int64_t resource_limits_manager::get_account_net_limit( const account_name& name, bool elastic) const {
   auto arl = get_account_net_limit_ex(name, elastic);
   return arl.available;
}

account_resource_limit resource_limits_manager::get_account_net_limit_ex( const account_name& name, bool elastic) const {
   const auto& config = _db.get<resource_limits_config_object>();
   const auto& state  = _db.get<resource_limits_state_object>();
   const auto& usage  = _db.get<resource_usage_object, by_owner>(name);

   int64_t net_weight, x, y;
   get_account_limits( name, x, net_weight, y );

   if( net_weight < 0 || state.total_net_weight == 0) {
      return { -1, -1, -1 };
   }

   account_resource_limit arl;

   uint128_t window_size = config.account_net_usage_average_window;

   uint128_t virtual_network_capacity_in_window = (uint128_t)(elastic ? state.virtual_net_limit : config.net_limit_parameters.max) * window_size;
   uint128_t user_weight     = (uint128_t)net_weight;
   uint128_t all_user_weight = (uint128_t)state.total_net_weight;


   auto max_user_use_in_window = (virtual_network_capacity_in_window * user_weight) / all_user_weight;
   auto net_used_in_window  = impl::integer_divide_ceil((uint128_t)usage.net_usage.value_ex * window_size, (uint128_t)config::rate_limiting_precision);

   if( max_user_use_in_window <= net_used_in_window )
      arl.available = 0;
   else
      arl.available = impl::downgrade_cast<int64_t>(max_user_use_in_window - net_used_in_window);

   arl.used = impl::downgrade_cast<int64_t>(net_used_in_window);
   arl.max = impl::downgrade_cast<int64_t>(max_user_use_in_window);
   return arl;
}

} } } /// gstio::chain::resource_limits
