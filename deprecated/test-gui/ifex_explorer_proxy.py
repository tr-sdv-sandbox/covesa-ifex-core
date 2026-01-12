#!/usr/bin/env python3
"""
IFEX Explorer REST Proxy
Provides REST API for the Explorer GUI to interact with IFEX services
Bridges gRPC service discovery to web GUI with CORS support
"""

from flask import Flask, jsonify, request, send_file
from flask_cors import CORS
import grpc
import sys
import os
import json
import time
import yaml
from datetime import datetime

# Add generated protobuf path
sys.path.append('../proto/python')

import service_discovery_service_pb2 as sd_pb2
import service_discovery_service_pb2_grpc as sd_grpc
import ifex_dispatcher_service_pb2 as dispatcher_pb2
import ifex_dispatcher_service_pb2_grpc as dispatcher_grpc

app = Flask(__name__)
CORS(app)  # Enable CORS for all routes

# Service discovery connection settings
SERVICE_DISCOVERY_HOST = os.getenv('SERVICE_DISCOVERY_HOST', 'localhost')
SERVICE_DISCOVERY_PORT = os.getenv('SERVICE_DISCOVERY_PORT', '50051')

def get_service_discovery_client():
    """Create gRPC client for service discovery"""
    channel = grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
    return sd_grpc.query_services_serviceStub(channel), channel

def get_dispatcher_client():
    """Create gRPC client for dispatcher service"""
    # Find dispatcher service via service discovery
    try:
        find_channel = grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
        find_stub = sd_grpc.get_service_serviceStub(find_channel)
        
        find_request = sd_pb2.get_service_request()
        find_request.service_name = "ifex-dispatcher"
        
        find_response = find_stub.get_service(find_request)
        find_channel.close()
        
        if find_response.service_info and find_response.service_info.name:
            dispatcher_address = find_response.service_info.endpoint.address
            print(f"🔗 Found dispatcher at: {dispatcher_address}")
            
            dispatcher_channel = grpc.insecure_channel(dispatcher_address)
            print(f"✅ Created dispatcher channel to {dispatcher_address}")
            return {
                'call_method': dispatcher_grpc.call_method_serviceStub(dispatcher_channel)
            }, dispatcher_channel
        else:
            print("❌ Dispatcher service not found")
            return None, None
            
    except Exception as e:
        print(f"❌ Failed to find dispatcher service: {e}")
        import traceback
        traceback.print_exc()
        return None, None

def convert_parameter_type(pb_type):
    """Convert protobuf parameter type enum to string"""
    type_mapping = {
        sd_pb2.UINT8: "UINT8",
        sd_pb2.UINT16: "UINT16", 
        sd_pb2.UINT32: "UINT32",
        sd_pb2.UINT64: "UINT64",
        sd_pb2.INT8: "INT8",
        sd_pb2.INT16: "INT16",
        sd_pb2.INT32: "INT32",
        sd_pb2.INT64: "INT64",
        sd_pb2.FLOAT: "FLOAT",
        sd_pb2.DOUBLE: "DOUBLE",
        sd_pb2.BOOLEAN: "BOOLEAN",
        sd_pb2.STRING: "STRING",
        sd_pb2.BYTES: "BYTES",
        sd_pb2.STRUCT: "STRUCT",
        sd_pb2.ARRAY: "ARRAY",
        sd_pb2.ENUM: "ENUM"
    }
    return type_mapping.get(pb_type, "UNKNOWN")

def get_struct_type_from_ifex(method_name, param_name, ifex_schema):
    """Extract the struct type name from IFEX schema for a given method parameter"""
    try:
        ifex_data = yaml.safe_load(ifex_schema)
        
        # Look for the method in IFEX schema
        for ns in ifex_data.get('namespaces', []):
            for method in ns.get('methods', []):
                if method['name'] == method_name:
                    # Look for the parameter in the method's input
                    for input_param in method.get('input', []):
                        if input_param['name'] == param_name:
                            return input_param['datatype']
        return None
    except Exception as e:
        print(f"Warning: Failed to get struct type for {method_name}.{param_name}: {e}")
        return None

def expand_struct_from_ifex(param_name, param_type, ifex_schema, visited_types=None):
    """Expand struct parameters using IFEX schema for GUI compatibility - handles nested structs"""
    if visited_types is None:
        visited_types = set()
    
    # Prevent infinite recursion
    if param_type in visited_types:
        return None
    visited_types.add(param_type)
    
    try:
        ifex_data = yaml.safe_load(ifex_schema)
        
        # Look for struct definition in IFEX
        for ns in ifex_data.get('namespaces', []):
            for struct in ns.get('structs', []):
                if struct['name'] == param_type:  # Match struct name
                    expanded_params = []
                    for member in struct.get('members', []):
                        member_type = member['datatype']
                        field_name = f"{param_name}.{member['name']}"
                        
                        # Check if this member is itself a struct - need to expand recursively
                        if member_type.endswith('_t') and not member_type.endswith('[]'):
                            # Check if it's a struct (not an enum)
                            is_struct = False
                            for ns2 in ifex_data.get('namespaces', []):
                                for struct2 in ns2.get('structs', []):
                                    if struct2['name'] == member_type:
                                        is_struct = True
                                        break
                                if is_struct:
                                    break
                            
                            if is_struct:
                                # Recursively expand nested struct
                                nested_params = expand_struct_from_ifex(field_name, member_type, ifex_schema, visited_types)
                                if nested_params:
                                    expanded_params.extend(nested_params)
                                    continue
                        
                        # Regular field handling
                        field = {
                            "name": field_name,
                            "description": member.get('description', ''),
                            "is_optional": not member.get('mandatory', True),
                            "min_value": None,
                            "max_value": None,
                            "enum_values": None,
                            "default_value": member.get('default')
                        }
                        
                        # Handle constraints
                        if 'constraints' in member:
                            constraints = member['constraints']
                            field["min_value"] = constraints.get('min')
                            field["max_value"] = constraints.get('max')
                            # Check for enum values in string constraints
                            if member_type == 'string' and 'values' in constraints:
                                field["enum_values"] = constraints['values']
                        
                        # Map types
                        if member_type == 'float':
                            field["type"] = "FLOAT"
                        elif 'uint' in member_type.lower():
                            field["type"] = "UINT16"
                        elif member_type == 'boolean':
                            field["type"] = "BOOLEAN"
                        elif member_type == 'string':
                            field["type"] = "STRING"
                        elif member_type.endswith('[]'):
                            field["type"] = "ARRAY"
                            # For arrays like zone_t[], look for enum values
                            if member_type.endswith('_t[]'):
                                base_type = member_type[:-2]
                                for enum_def in ns.get('enumerations', []):
                                    if enum_def['name'] == base_type:
                                        field["enum_values"] = [f"{opt['name']} ({opt['value']})" for opt in enum_def.get('options', [])]
                                        # Keep array defaults as numeric - GUI expects numeric values
                                        break
                        elif member_type.endswith('_t'):
                            # Handle enum types like preconditioning_mode_t
                            field["type"] = "UINT16"  # Enums are numeric
                            for enum_def in ns.get('enumerations', []):
                                if enum_def['name'] == member_type:
                                    field["enum_values"] = [f"{opt['name']} ({opt['value']})" for opt in enum_def.get('options', [])]
                                    # Keep numeric default for enum types - GUI expects numeric values
                                    # The GUI will match this against the enum values to show the default
                                    break
                        else:
                            field["type"] = "STRING"
                            
                        expanded_params.append(field)
                    return expanded_params
    except Exception as e:
        print(f"Warning: Failed to expand struct {param_type}: {e}")
    
    return None

def extract_struct_definitions(ifex_schema):
    """Extract all struct definitions from IFEX schema"""
    struct_defs = {}
    try:
        ifex_data = yaml.safe_load(ifex_schema)
        
        for ns in ifex_data.get('namespaces', []):
            # Extract enumerations first (for reference)
            enum_defs = {}
            for enum_def in ns.get('enumerations', []):
                enum_defs[enum_def['name']] = {
                    'type': 'enum',
                    'base_type': enum_def.get('datatype', 'uint16'),
                    'values': [f"{opt['name']} ({opt['value']})" for opt in enum_def.get('options', [])]
                }
            
            # Extract struct definitions
            for struct_def in ns.get('structs', []):
                struct_info = {
                    'name': struct_def['name'],
                    'description': struct_def.get('description', ''),
                    'members': []
                }
                
                for member in struct_def.get('members', []):
                    member_type = member.get('datatype', 'STRING')
                    member_info = {
                        'name': member['name'],
                        'datatype': member_type,
                        'description': member.get('description', ''),
                        'is_optional': not member.get('mandatory', True),
                        'default_value': member.get('default')
                    }
                    
                    # Handle constraints
                    if 'constraints' in member:
                        member_info['constraints'] = member['constraints']
                        # Check if string has enum values in constraints
                        if member_type == 'string' and 'values' in member['constraints']:
                            member_info['enum_values'] = member['constraints']['values']
                    
                    # Map type information
                    if member_type in ['float', 'double']:
                        member_info['type'] = 'FLOAT' if member_type == 'float' else 'DOUBLE'
                    elif member_type in ['uint8', 'uint16', 'uint32', 'uint64']:
                        member_info['type'] = member_type.upper()
                    elif member_type in ['int8', 'int16', 'int32', 'int64']:
                        member_info['type'] = member_type.upper()
                    elif member_type == 'boolean':
                        member_info['type'] = 'BOOLEAN'
                    elif member_type == 'string':
                        member_info['type'] = 'STRING'
                    elif member_type.endswith('[]'):
                        member_info['type'] = 'ARRAY'
                        base_type = member_type[:-2]
                        if base_type in enum_defs:
                            member_info['enum_values'] = enum_defs[base_type]['values']
                    elif member_type in enum_defs:
                        # It's an enum type
                        member_info['type'] = 'ENUM'
                        member_info['enum_values'] = enum_defs[member_type]['values']
                    elif member_type.endswith('_t'):
                        # It's a struct type
                        member_info['type'] = 'STRUCT'
                        member_info['struct_type'] = member_type
                    else:
                        member_info['type'] = 'STRING'  # Default
                    
                    struct_info['members'].append(member_info)
                
                struct_defs[struct_def['name']] = struct_info
                
    except Exception as e:
        print(f"Warning: Failed to extract struct definitions: {e}")
    
    return struct_defs

def convert_service_to_json(service):
    """Convert protobuf service to JSON format using metadata from service discovery"""
    service_data = {
        "name": service.name,
        "version": service.version,
        "description": service.description,
        "address": service.endpoint.address,
        "transport": "gRPC",
        "status": "AVAILABLE" if service.status == sd_pb2.AVAILABLE else "UNAVAILABLE",
        "last_heartbeat": service.last_heartbeat,
        "namespaces": [],
        "ifex_schema": service.ifex_schema if hasattr(service, 'ifex_schema') else None
    }
    
    # If no namespaces from service discovery but IFEX schema is available, parse it
    if len(service.namespaces) == 0 and service.ifex_schema:
        try:
            ifex_data = yaml.safe_load(service.ifex_schema)
            for ns in ifex_data.get('namespaces', []):
                # Build enum lookup first
                enum_names = set()
                for enum_def in ns.get('enumerations', []):
                    enum_names.add(enum_def['name'])
                namespace_data = {
                    "name": ns.get('name', ''),
                    "description": ns.get('description', ''),
                    "methods": []
                }
                
                for method in ns.get('methods', []):
                    method_data = {
                        "name": method.get('name', ''),
                        "description": method.get('description', ''),
                        "is_schedulable": False,  # Not a standard IFEX field, default to False
                        "max_execution_time_ms": 0,  # Not a standard IFEX field, default to 0
                        "input_parameters": [],
                        "output_parameters": []
                    }
                    
                    # Parse input parameters from IFEX
                    for param in method.get('input', []):
                        # Convert IFEX datatype to uppercase format expected by GUI
                        datatype = param.get('datatype', 'string')
                        if datatype in ['float', 'double', 'boolean', 'string']:
                            datatype = datatype.upper()
                        elif datatype.startswith(('uint', 'int')):
                            datatype = datatype.upper()
                        elif datatype.endswith('_t'):
                            # Check if it's a struct or enum
                            if datatype in enum_names:
                                datatype = 'ENUM'
                            else:
                                datatype = 'STRUCT'
                        
                        param_data = {
                            "name": param.get('name', ''),
                            "type": datatype,
                            "description": param.get('description', ''),
                            "is_optional": not param.get('mandatory', True),
                            "min_value": param.get('constraints', {}).get('min'),
                            "max_value": param.get('constraints', {}).get('max'),
                            "enum_values": None,
                            "default_value": param.get('default')
                        }
                        
                        # If it's a struct, add the struct_type
                        if datatype == 'STRUCT':
                            param_data['struct_type'] = param.get('datatype', '')
                            print(f"DEBUG: Setting struct_type for {param.get('name')}: {param.get('datatype')}")
                        
                        # If it's an enum, add enum values
                        if datatype == 'ENUM':
                            original_type = param.get('datatype', '')
                            for enum_def in ns.get('enumerations', []):
                                if enum_def['name'] == original_type:
                                    param_data['enum_values'] = [f"{opt['name']} ({opt['value']})" for opt in enum_def.get('options', [])]
                                    break
                        
                        method_data["input_parameters"].append(param_data)
                    
                    # Parse output parameters from IFEX
                    for param in method.get('output', []):
                        # Convert IFEX datatype to uppercase format expected by GUI
                        datatype = param.get('datatype', 'string')
                        if datatype in ['float', 'double', 'boolean', 'string']:
                            datatype = datatype.upper()
                        elif datatype.startswith(('uint', 'int')):
                            datatype = datatype.upper()
                        elif datatype.endswith('_t'):
                            # Check if it's a struct or enum
                            if datatype in enum_names:
                                datatype = 'ENUM'
                            else:
                                datatype = 'STRUCT'
                        
                        param_data = {
                            "name": param.get('name', ''),
                            "type": datatype,
                            "description": param.get('description', ''),
                            "is_optional": not param.get('mandatory', True),
                            "min_value": None,
                            "max_value": None,
                            "enum_values": None
                        }
                        method_data["output_parameters"].append(param_data)
                    
                    namespace_data["methods"].append(method_data)
                
                service_data["namespaces"].append(namespace_data)
        except Exception as e:
            print(f"Failed to parse IFEX schema for {service.name}: {e}")
    
    # Otherwise use pre-parsed metadata from service discovery
    else:
        for namespace in service.namespaces:
            namespace_data = {
                "name": namespace.name,
                "description": namespace.description,
                "methods": []
            }
            
            for method in namespace.methods:
                method_data = {
                    "name": method.name,
                    "description": method.description,
                    "is_schedulable": method.is_schedulable if hasattr(method, 'is_schedulable') else False,
                    "max_execution_time_ms": method.max_execution_time_ms if hasattr(method, 'max_execution_time_ms') else 0,
                    "input_parameters": [],
                    "output_parameters": []
                }
                
                # Convert input parameters
                for param in method.input_parameters:
                    param_type = convert_parameter_type(param.type)
                    param_data = {
                        "name": param.name,
                        "type": param_type,
                        "description": param.description,
                        "is_optional": param.is_optional,
                        "min_value": param.min_value if param.min_value else None,
                        "max_value": param.max_value if param.max_value else None,
                        "enum_values": list(param.enum_values) if param.enum_values else None,
                        "default_value": None
                    }
                    
                    # If it's a struct and we have IFEX schema, get the struct type name
                    if param_type == "STRUCT" and service.ifex_schema:
                        struct_type = get_struct_type_from_ifex(method.name, param.name, service.ifex_schema)
                        if struct_type:
                            param_data["struct_type"] = struct_type
                            print(f"DEBUG: Setting struct_type for {param.name}: {struct_type}")
                    
                    method_data["input_parameters"].append(param_data)
                
                # Convert output parameters
                for param in method.output_parameters:
                    param_type = convert_parameter_type(param.type)
                    param_data = {
                        "name": param.name,
                        "type": param_type,
                        "description": param.description,
                        "is_optional": param.is_optional,
                        "min_value": param.min_value if param.min_value else None,
                        "max_value": param.max_value if param.max_value else None,
                        "enum_values": list(param.enum_values) if param.enum_values else None
                    }
                    
                    # If it's a struct and we have IFEX schema, get the struct type name
                    if param_type == "STRUCT" and service.ifex_schema:
                        struct_type = get_struct_type_from_ifex(method.name, param.name, service.ifex_schema)
                        if struct_type:
                            param_data["struct_type"] = struct_type
                    
                    method_data["output_parameters"].append(param_data)
                
                namespace_data["methods"].append(method_data)
            
            service_data["namespaces"].append(namespace_data)
    
    # Extract struct definitions from IFEX schema
    if service.ifex_schema:
        service_data["struct_definitions"] = extract_struct_definitions(service.ifex_schema)
    else:
        service_data["struct_definitions"] = {}
    
    return service_data

@app.route('/api/services', methods=['GET'])
def list_services():
    """List all available services from service discovery"""
    try:
        print("📡 Fetching services from service discovery...")
        
        # Connect to service discovery
        stub, channel = get_service_discovery_client()
        
        # Create request
        request = sd_pb2.query_services_request()
        request.filter.transport_type = sd_pb2.transport_type_t.GRPC
        
        # Get services  
        response = stub.query_services(request)
        print(f"✅ Found {len(response.services)} services")
        
        # Convert to JSON format
        services = []
        
        # Get full details for each service using get_service
        get_channel = grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
        get_stub = sd_grpc.get_service_serviceStub(get_channel)
        
        for service in response.services:
            try:
                # Get full service details
                get_request = sd_pb2.get_service_request()
                get_request.service_name = service.name
                get_response = get_stub.get_service(get_request)
                
                if get_response.service_info and get_response.service_info.name:
                    service = get_response.service_info  # Use full service info
                    print(f"\n📋 Got full info for {service.name}")
            except Exception as e:
                print(f"\n⚠️  Failed to get full info for {service.name}: {e}")
            service_json = convert_service_to_json(service)
            
            # Print service summary
            print(f"\n📦 Service: {service.name}")
            print(f"   Version: {service.version}")
            print(f"   Address: {service.endpoint.address}")
            print(f"   Status: {service_json['status']}")
            print(f"   Namespaces: {len(service.namespaces)}")
            
            # Debug namespace details
            for idx, ns in enumerate(service.namespaces):
                print(f"   Namespace[{idx}]: name='{ns.name}', methods={len(ns.methods)}")
            
            # Check if IFEX schema is available
            if hasattr(service, 'ifex_schema') and service.ifex_schema:
                print(f"   IFEX Schema available: {len(service.ifex_schema)} bytes")
                # Log first 200 chars of schema for debugging
                print(f"   IFEX Schema preview: {service.ifex_schema[:200]}...")
                # Try parsing the IFEX schema to get namespaces
                try:
                    import yaml
                    ifex_data = yaml.safe_load(service.ifex_schema)
                    if 'namespaces' in ifex_data:
                        print(f"   IFEX Schema contains {len(ifex_data['namespaces'])} namespaces")
                        for ns in ifex_data['namespaces']:
                            methods = ns.get('methods', [])
                            print(f"     - Namespace '{ns.get('name', 'unnamed')}' has {len(methods)} methods")
                except Exception as e:
                    print(f"   Failed to parse IFEX schema: {e}")
            else:
                print(f"   No IFEX Schema in service_info")
            
            # Count schedulable methods
            schedulable_count = 0
            total_methods = 0
            for ns in service.namespaces:
                total_methods += len(ns.methods)
                for method in ns.methods:
                    if hasattr(method, 'is_schedulable') and method.is_schedulable:
                        schedulable_count += 1
                        print(f"     ⏰ {method.name} (schedulable)")
            
            print(f"   📊 Total methods: {total_methods}, Schedulable: {schedulable_count}")
            
            services.append(service_json)
        
        # Close channels
        get_channel.close()
        channel.close()
        
        return jsonify({
            "services": services,
            "count": len(services)
        })
        
    except grpc.RpcError as e:
        print(f"❌ gRPC Error: {e.code()} - {e.details()}")
        return jsonify({
            "error": f"Failed to connect to service discovery: {e.details()}"
        }), 503
    except Exception as e:
        print(f"❌ Error: {str(e)}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/api/services/<service_name>', methods=['GET'])
def get_service_details(service_name):
    """Get details for a specific service"""
    try:
        print(f"🔍 Looking up service: {service_name}")
        
        # Connect to service discovery
        stub = sd_grpc.get_service_serviceStub(
            grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
        )
        
        # Create request
        request = sd_pb2.get_service_request()
        request.service_name = service_name
        
        # Find service
        response = stub.get_service(request)
        
        if response.service_info and response.service_info.name:
            service = response.service_info
            print(f"✅ Found service at {service.endpoint.address}")
            
            service_json = convert_service_to_json(service)
            
            # Debug log the execute method if this is the orchestration service
            if service_name == "morning_commute_orchestration":
                print(f"\n🔍 DEBUG: Orchestration service JSON structure:")
                print(f"   Has struct_definitions: {'struct_definitions' in service_json}")
                if 'struct_definitions' in service_json:
                    print(f"   Number of struct definitions: {len(service_json['struct_definitions'])}")
                    print(f"   Struct names: {list(service_json['struct_definitions'].keys())}")
                
                # Check execute method
                for ns in service_json.get('namespaces', []):
                    for method in ns.get('methods', []):
                        if method['name'] == 'execute':
                            print(f"\n   Execute method structure:")
                            for param in method.get('input_parameters', []):
                                print(f"     - {param['name']}: type={param.get('type')}, struct_type={param.get('struct_type', 'NOT SET')}")
            
            return jsonify({
                "status": "success",
                "service": service_json
            })
        else:
            print(f"❌ Service not found: {service_name}")
            return jsonify({
                "status": "error",
                "message": f"Service '{service_name}' not found"
            }), 404
            
    except grpc.RpcError as e:
        print(f"❌ gRPC Error: {e.code()} - {e.details()}")
        return jsonify({
            "status": "error",
            "message": f"Failed to find service: {e.details()}"
        }), 503
    except Exception as e:
        print(f"❌ Error: {str(e)}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/api/services/<service_name>/methods/<method_name>', methods=['GET'])
def get_method_details(service_name, method_name):
    """Get detailed information about a specific method"""
    try:
        print(f"🔍 Getting method details: {service_name}.{method_name}")
        
        # First get the service details to find the method
        stub = sd_grpc.find_service_serviceStub(
            grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
        )
        
        request = sd_pb2.find_service_request()
        request.service_name = service_name
        response = stub.find_service(request)
        
        if not response.services:
            return jsonify({
                "status": "error",
                "message": f"Service '{service_name}' not found"
            }), 404
        
        service = response.services[0]
        
        # Find the method in the service's namespaces
        method_found = None
        for namespace in service.namespaces:
            for method in namespace.methods:
                if method.name == method_name:
                    method_found = method
                    break
            if method_found:
                break
        
        if not method_found:
            return jsonify({
                "status": "error",
                "message": f"Method '{method_name}' not found in service '{service_name}'"
            }), 404
        
        # Convert method details to JSON
        method_data = {
            "name": method_found.name,
            "description": method_found.description,
            "is_schedulable": method_found.is_schedulable if hasattr(method_found, 'is_schedulable') else False,
            "max_execution_time_ms": method_found.max_execution_time_ms if hasattr(method_found, 'max_execution_time_ms') else 0,
            "input_parameters": [],
            "output_parameters": []
        }
        
        # Convert input parameters with struct expansion
        for param in method_found.input_parameters:
            param_type = convert_parameter_type(param.type)
            
            # Check if this is a struct type that should be sent as struct definition
            if param_type == "STRUCT" and service.ifex_schema:
                struct_name = get_struct_type_from_ifex(method_name, param.name, service.ifex_schema)
                if struct_name:
                    # Only add the struct parameter itself - GUI will use struct definitions to render it
                    struct_param = {
                        "name": param.name,
                        "type": "STRUCT",
                        "struct_type": struct_name,
                        "description": param.description,
                        "is_optional": param.is_optional,
                        "min_value": None,
                        "max_value": None,
                        "enum_values": None,
                        "default_value": None
                    }
                    method_data["input_parameters"].append(struct_param)
                    continue
            
            # Regular parameter
            param_data = {
                "name": param.name,
                "type": param_type,
                "description": param.description,
                "is_optional": param.is_optional,
                "min_value": param.min_value if param.min_value else None,
                "max_value": param.max_value if param.max_value else None,
                "enum_values": list(param.enum_values) if param.enum_values else None,
                "default_value": None
            }
            method_data["input_parameters"].append(param_data)
        
        # Convert output parameters
        for param in method_found.output_parameters:
            param_data = {
                "name": param.name,
                "type": convert_parameter_type(param.type),
                "description": param.description,
                "is_optional": param.is_optional,
                "min_value": param.min_value if param.min_value else None,
                "max_value": param.max_value if param.max_value else None,
                "enum_values": list(param.enum_values) if param.enum_values else None
            }
            method_data["output_parameters"].append(param_data)
        
        return jsonify({
            "status": "success",
            "method": method_data,
            "service_address": service.endpoint.address
        })
        
    except Exception as e:
        print(f"❌ Error getting method details: {str(e)}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

@app.route('/api/services/<service_name>/methods/<method_name>/call', methods=['POST'])
def call_method(service_name, method_name):
    """Call a service method via dispatcher"""
    try:
        data = request.get_json()
        parameters = data.get('parameters', {})
        timeout_ms = data.get('timeout_ms', 5000)
        
        print(f"📞 Calling {service_name}.{method_name} via dispatcher")
        print(f"   Parameters: {json.dumps(parameters, indent=2)}")
        
        # Log to file for debugging
        import logging
        logging.basicConfig(filename='explorer_proxy_debug.log', level=logging.DEBUG)
        logging.debug(f"Call to {service_name}.{method_name} with params: {json.dumps(parameters, indent=2)}")
        
        # Get dispatcher client
        dispatcher_stubs, dispatcher_channel = get_dispatcher_client()
        if not dispatcher_stubs:
            return jsonify({
                "status": "error",
                "message": "Dispatcher service not available"
            }), 503
        
        try:
            # Create dispatcher request
            call_request = dispatcher_pb2.call_method_request()
            call_request.call.service_name = service_name
            call_request.call.method_name = method_name
            call_request.call.parameters = json.dumps(parameters)
            call_request.call.timeout_ms = timeout_ms
            
            # Call the dispatcher
            response = dispatcher_stubs['call_method'].call_method(call_request)
            
            # Map dispatcher status to response
            status_map = {
                dispatcher_pb2.SUCCESS: "success",
                dispatcher_pb2.FAILED: "failed",
                dispatcher_pb2.TIMEOUT: "timeout",
                dispatcher_pb2.SERVICE_UNAVAILABLE: "service_unavailable",
                dispatcher_pb2.METHOD_NOT_FOUND: "method_not_found",
                dispatcher_pb2.INVALID_PARAMETERS: "invalid_parameters"
            }
            
            result = {
                "status": status_map.get(response.result.status, "unknown"),
                "duration_ms": response.result.duration_ms,
                "service_endpoint": response.result.service_endpoint
            }
            
            if response.result.status == dispatcher_pb2.SUCCESS:
                try:
                    result["result"] = json.loads(response.result.response) if response.result.response else {}
                except json.JSONDecodeError:
                    result["result"] = {"raw_response": response.result.response}
                    
                print(f"✅ Method call successful, duration: {response.result.duration_ms}ms")
            else:
                result["error"] = response.result.error_message
                print(f"❌ Method call failed: {response.result.error_message}")
            
            return jsonify(result)
            
        finally:
            dispatcher_channel.close()
        
    except Exception as e:
        print(f"❌ Error: {str(e)}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

@app.route('/ifex_explorer_gui.html', methods=['GET'])
def serve_gui():
    """Serve the GUI HTML file"""
    try:
        with open('ifex_explorer_gui.html', 'r') as f:
            return f.read(), 200, {'Content-Type': 'text/html'}
    except FileNotFoundError:
        return "GUI file not found", 404

@app.route('/health', methods=['GET'])
@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({"status": "healthy", "service": "ifex-explorer-proxy"})

if __name__ == '__main__':
    port = int(os.getenv('PROXY_PORT', '5002'))
    print(f"""
    🌐 IFEX Explorer REST Proxy
    ===========================
    Service Discovery: {SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}
    REST API Port: {port}
    
    Endpoints:
    - GET  /api/services           - List all services
    - GET  /api/services/<name>    - Get service details
    - GET  /api/services/<name>/methods/<method> - Get method details
    - POST /api/services/<name>/methods/<method>/call - Call service method
    - GET  /health                 - Health check
    
    GUI:
    - GET  /ifex_explorer_gui.html - Explorer web interface
    
    Starting Flask server...
    """)
    
    app.run(host='0.0.0.0', port=port, debug=True)