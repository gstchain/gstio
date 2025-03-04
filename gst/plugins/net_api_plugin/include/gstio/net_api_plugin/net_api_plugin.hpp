/**
 *  @file
 *  @copyright defined in gst/LICENSE
 */
#pragma once

#include <gstio/net_plugin/net_plugin.hpp>
#include <gstio/http_plugin/http_plugin.hpp>

#include <appbase/application.hpp>

namespace gstio {

using namespace appbase;

class net_api_plugin : public plugin<net_api_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((net_plugin) (http_plugin))

   net_api_plugin() = default;
   net_api_plugin(const net_api_plugin&) = delete;
   net_api_plugin(net_api_plugin&&) = delete;
   net_api_plugin& operator=(const net_api_plugin&) = delete;
   net_api_plugin& operator=(net_api_plugin&&) = delete;
   virtual ~net_api_plugin() override = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override {}
   void plugin_initialize(const variables_map& vm);
   void plugin_startup();
   void plugin_shutdown() {}

private:
};

}
