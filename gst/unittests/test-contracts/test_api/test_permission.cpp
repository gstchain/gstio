/**
 * @file action_test.cpp
 * @copyright defined in gst/LICENSE
 */
#include <limits>

#include <gstiolib/action.hpp>
#include <gstiolib/db.h>
#include <gstiolib/gstio.hpp>
#include <gstiolib/permission.h>
#include <gstiolib/print.hpp>
#include <gstiolib/serialize.hpp>

#include "test_api.hpp"



struct check_auth_msg {
   gstio::name                    account;
   gstio::name                    permission;
   std::vector<gstio::public_key> pubkeys;

   GSTLIB_SERIALIZE( check_auth_msg, (account)(permission)(pubkeys)  )
};

void test_permission::check_authorization( uint64_t receiver, uint64_t code, uint64_t action ) {
   (void)code;
   (void)action;
   using namespace gstio;

   auto self = receiver;
   auto params = unpack_action_data<check_auth_msg>();
   auto packed_pubkeys = pack(params.pubkeys);
   int64_t res64 = ::check_permission_authorization( params.account.value,
                                                     params.permission.value,
                                                     packed_pubkeys.data(), packed_pubkeys.size(),
                                                     (const char*)0,        0,
                                                     static_cast<uint64_t>( std::numeric_limits<int64_t>::max() )
                                                   );

   auto itr = db_lowerbound_i64( self, self, self, 1 );
   if(itr == -1) {
      db_store_i64( self, self, self, 1, &res64, sizeof(int64_t) );
   } else {
      db_update_i64( itr, self, &res64, sizeof(int64_t) );
   }
}

struct test_permission_last_used_msg {
   gstio::name account;
   gstio::name permission;
   int64_t     last_used_time;

   GSTLIB_SERIALIZE( test_permission_last_used_msg, (account)(permission)(last_used_time) )
};

void test_permission::test_permission_last_used( uint64_t /* receiver */, uint64_t code, uint64_t action ) {
   (void)code;
   (void)action;
   using namespace gstio;

   auto params = unpack_action_data<test_permission_last_used_msg>();

   gstio_assert( get_permission_last_used(params.account.value, params.permission.value) == params.last_used_time, "unexpected last used permission time" );
}

void test_permission::test_account_creation_time( uint64_t /* receiver */, uint64_t code, uint64_t action ) {
   (void)code;
   (void)action;
   using namespace gstio;

   auto params = unpack_action_data<test_permission_last_used_msg>();

   gstio_assert( get_account_creation_time(params.account.value) == params.last_used_time, "unexpected account creation time" );
}
