/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <osquery/enroll.h>
#include <osquery/flags.h>
#include <osquery/registry.h>

#include "osquery/remote/requests.h"
#include "osquery/remote/serializers/json.h"
#include "osquery/remote/transports/tls.h"
#include "osquery/remote/utility.h"

#include "osquery/config/parsers/decorators.h"
#include "osquery/logger/plugins/tls.h"

namespace pt = boost::property_tree;

namespace osquery {

constexpr size_t kTLSMaxLogLines = 1024;

FLAG(string, logger_tls_endpoint, "", "TLS/HTTPS endpoint for results logging");

FLAG(uint64,
     logger_tls_period,
     4,
     "Seconds between flushing logs over TLS/HTTPS");

FLAG(uint64,
     logger_tls_max,
     1 * 1024 * 1024,
     "Max size in bytes allowed per log line");

FLAG(bool, logger_tls_compress, false, "GZip compress TLS/HTTPS request body");

REGISTER(TLSLoggerPlugin, "logger", "tls");

TLSLogForwarder::TLSLogForwarder(const std::string& node_key)
    : BufferedLogForwarder("tls",
                           std::chrono::seconds(FLAGS_logger_tls_period),
                           kTLSMaxLogLines),
      node_key_(node_key) {
  uri_ = TLSRequestHelper::makeURI(FLAGS_logger_tls_endpoint);
}

Status TLSLoggerPlugin::logString(const std::string& s) {
  return forwarder_->logString(s);
}

Status TLSLoggerPlugin::logStatus(const std::vector<StatusLogLine>& log) {
  return forwarder_->logStatus(log);
}

Status TLSLoggerPlugin::setUp() {
  auto node_key = getNodeKey("tls");
  if (!FLAGS_disable_enrollment && node_key.size() == 0) {
    // Could not generate a node key, continue logging to stderr.
    return Status(1, "No node key, TLS logging disabled.");
  }
  // Start the log forwarding/flushing thread.
  forwarder_ = std::make_shared<TLSLogForwarder>(node_key);
  Dispatcher::addService(forwarder_);
  return Status(0);
}

Status TLSLoggerPlugin::init(const std::string& name,
                             const std::vector<StatusLogLine>& log) {
  // Restart the glog facilities using the name init was provided.
  google::ShutdownGoogleLogging();
  google::InitGoogleLogging(name.c_str());
  return logStatus(log);
}

Status TLSLogForwarder::send(std::vector<std::string>& log_data,
                             const std::string& log_type) {
  pt::ptree params;
  params.put<std::string>("node_key", node_key_);
  params.put<std::string>("log_type", log_type);

  {
    // Read each logged line into JSON and populate a list of lines.
    // The result list will use the 'data' key.
    pt::ptree children;
    iterate(log_data,
            ([&children](std::string& item) {
              // Enforce a max log line size for TLS logging.
              if (item.size() > FLAGS_logger_tls_max) {
                LOG(WARNING) << "Line exceeds TLS logger max: " << item.size();
                return;
              }

              pt::ptree child;
              try {
                std::stringstream input;
                input << item;
                std::string().swap(item);
                pt::read_json(input, child);
              } catch (const pt::json_parser::json_parser_error& e) {
                // The log line entered was not valid JSON, skip it.
                return;
              }
              children.push_back(std::make_pair("", std::move(child)));
            }));
    params.add_child("data", std::move(children));
  }

  auto request = Request<TLSTransport, JSONSerializer>(uri_);
  request.setOption("hostname", FLAGS_tls_hostname);
  if (FLAGS_logger_tls_compress) {
    request.setOption("compress", true);
  }
  return request.call(params);
}

}
