#pragma once

#include <boost/describe.hpp>

#include <string>
#include <vector>

namespace forge::otlp {

struct otlp_string_value {
   std::string stringValue;
};

struct otlp_attribute {
   std::string key;
   otlp_string_value value;
};

struct otlp_resource {
   std::vector<otlp_attribute> attributes;
};

struct otlp_scope {
   std::string name;
   std::string version;
};

struct otlp_log_body {
   std::string stringValue;
};

struct otlp_log_record {
   std::string timeUnixNano;
   int severityNumber = 0;
   std::string severityText;
   otlp_log_body body;
   std::vector<otlp_attribute> attributes;
};

struct otlp_scope_log {
   otlp_scope scope;
   std::vector<otlp_log_record> logRecords;
};

struct otlp_resource_log {
   otlp_resource resource;
   std::vector<otlp_scope_log> scopeLogs;
};

struct otlp_export_request {
   std::vector<otlp_resource_log> resourceLogs;
};

BOOST_DESCRIBE_STRUCT(otlp_string_value, (), (stringValue))
BOOST_DESCRIBE_STRUCT(otlp_attribute, (), (key, value))
BOOST_DESCRIBE_STRUCT(otlp_resource, (), (attributes))
BOOST_DESCRIBE_STRUCT(otlp_scope, (), (name, version))
BOOST_DESCRIBE_STRUCT(otlp_log_body, (), (stringValue))
BOOST_DESCRIBE_STRUCT(otlp_log_record, (), (timeUnixNano, severityNumber, severityText, body, attributes))
BOOST_DESCRIBE_STRUCT(otlp_scope_log, (), (scope, logRecords))
BOOST_DESCRIBE_STRUCT(otlp_resource_log, (), (resource, scopeLogs))
BOOST_DESCRIBE_STRUCT(otlp_export_request, (), (resourceLogs))

} // namespace forge::otlp
