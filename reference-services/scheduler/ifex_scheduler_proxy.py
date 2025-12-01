#!/usr/bin/env python3
"""
IFEX Scheduler REST Proxy
Provides REST API for the Scheduler GUI to interact with IFEX services
Bridges gRPC service discovery and scheduler to web GUI with CORS support
"""

from flask import Flask, jsonify, request
from flask_cors import CORS
import grpc
import sys
import os
import json
import time
import yaml
from datetime import datetime

# Add generated protobuf path
sys.path.append('../generated/python')

import service_discovery_service_pb2 as sd_pb2
import service_discovery_service_pb2_grpc as sd_grpc
import ifex_scheduler_service_pb2 as sched_pb2
import ifex_scheduler_service_pb2_grpc as sched_grpc

app = Flask(__name__)
CORS(app)  # Enable CORS for all routes

# Service discovery connection settings
SERVICE_DISCOVERY_HOST = os.getenv('SERVICE_DISCOVERY_HOST', 'localhost')
SERVICE_DISCOVERY_PORT = os.getenv('SERVICE_DISCOVERY_PORT', '50051')

def get_service_discovery_client():
    """Create gRPC client for service discovery"""
    channel = grpc.insecure_channel(f'{SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}')
    return sd_grpc.query_services_serviceStub(channel), channel

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

def get_struct_definitions_from_ifex(ifex_schema):
    """Extract struct definitions from IFEX schema"""
    struct_defs = {}
    try:
        ifex_data = yaml.safe_load(ifex_schema)
        for ns in ifex_data.get('namespaces', []):
            for struct in ns.get('structs', []):
                struct_name = struct['name']
                struct_defs[struct_name] = {
                    'name': struct_name,
                    'description': struct.get('description', ''),
                    'members': []
                }
                
                for member in struct.get('members', []):
                    member_info = {
                        'name': member['name'],
                        'type': member['datatype'],
                        'description': member.get('description', ''),
                        'mandatory': member.get('mandatory', True),
                        'default': member.get('default')
                    }
                    
                    # Handle constraints
                    if 'constraints' in member:
                        member_info['min'] = member['constraints'].get('min')
                        member_info['max'] = member['constraints'].get('max')
                    
                    struct_defs[struct_name]['members'].append(member_info)
    except Exception as e:
        print(f"Warning: Failed to get struct definitions: {e}")
    
    return struct_defs

def get_enum_definitions_from_ifex(ifex_schema):
    """Extract enum definitions from IFEX schema"""
    enum_defs = {}
    try:
        ifex_data = yaml.safe_load(ifex_schema)
        for ns in ifex_data.get('namespaces', []):
            for enum in ns.get('enumerations', []):
                enum_name = enum['name']
                enum_defs[enum_name] = {
                    'name': enum_name,
                    'description': enum.get('description', ''),
                    'options': []
                }
                
                for opt in enum.get('options', []):
                    enum_defs[enum_name]['options'].append({
                        'name': opt['name'],
                        'value': opt['value'],
                        'description': opt.get('description', '')
                    })
    except Exception as e:
        print(f"Warning: Failed to get enum definitions: {e}")
    
    return enum_defs

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
        "ifex_schema": service.ifex_schema if hasattr(service, 'ifex_schema') else None,
        "struct_definitions": {},
        "enum_definitions": {}
    }
    
    # Extract struct and enum definitions if IFEX schema is available
    if service.ifex_schema:
        service_data["struct_definitions"] = get_struct_definitions_from_ifex(service.ifex_schema)
        service_data["enum_definitions"] = get_enum_definitions_from_ifex(service.ifex_schema)
    
    # Parse IFEX schema to get enhanced scheduling metadata if available
    enhanced_methods = {}
    if service.ifex_schema:
        try:
            ifex_data = yaml.safe_load(service.ifex_schema)
            for ns in ifex_data.get('namespaces', []):
                for method in ns.get('methods', []):
                    if 'x-scheduling' in method:
                        enhanced_methods[method['name']] = method['x-scheduling']
        except Exception as e:
            print(f"Warning: Failed to parse enhanced scheduling metadata: {e}")
    
    # Use pre-parsed metadata from service discovery
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
                # is_schedulable removed - use x-scheduling metadata instead
                "max_execution_time_ms": method.max_execution_time_ms,
                "input_parameters": [],
                "output_parameters": []
            }
            
            # Add enhanced scheduling metadata if available
            if method.name in enhanced_methods:
                method_data["x_scheduling"] = enhanced_methods[method.name]
            
            # Convert input parameters - NO FLATTENING
            for param in method.input_parameters:
                param_type = convert_parameter_type(param.type)
                
                # Check if this is a struct type
                struct_name = None
                if param_type == "STRUCT" and service.ifex_schema:
                    # Get the struct type name from the IFEX schema
                    struct_name = get_struct_type_from_ifex(method.name, param.name, service.ifex_schema)
                
                # Regular parameter data
                param_data = {
                    "name": param.name,
                    "type": param_type,
                    "struct_type": struct_name if struct_name else None,
                    "description": param.description,
                    "is_optional": param.is_optional,
                    "min_value": param.min_value if param.min_value else None,
                    "max_value": param.max_value if param.max_value else None,
                    "enum_values": list(param.enum_values) if param.enum_values else None,
                    "default_value": None
                }
                
                # Try to get default value from IFEX schema
                if service.ifex_schema:
                    try:
                        ifex_data = yaml.safe_load(service.ifex_schema)
                        for ns in ifex_data.get('namespaces', []):
                            for method_def in ns.get('methods', []):
                                if method_def['name'] == method.name:
                                    for input_def in method_def.get('input', []):
                                        if input_def['name'] == param.name:
                                            default_val = input_def.get('default')
                                            if default_val is not None:
                                                param_data["default_value"] = default_val
                                            break
                                    break
                    except:
                        pass
                
                method_data["input_parameters"].append(param_data)
            
            # Convert output parameters
            for param in method.output_parameters:
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
            
            namespace_data["methods"].append(method_data)
        
        service_data["namespaces"].append(namespace_data)
    
    return service_data

@app.route('/api/services', methods=['GET'])
def query_services():
    """List all available services from service discovery"""
    try:
        print("📡 Fetching services from service discovery...")
        
        # Connect to service discovery
        stub, channel = get_service_discovery_client()
        
        # Create request
        request = sd_pb2.query_services_request()
        request.filter.transport_type = sd_pb2.transport_type_t.GRPC
        # Filter for services with schedulable methods
        request.filter.extension_paths.append("x-scheduling.enabled=true")
        
        # Get services  
        response = stub.query_services(request)
        print(f"✅ Found {len(response.services)} services")
        
        # Convert to JSON format
        services = []
        for service in response.services:
            service_json = convert_service_to_json(service)
            
            # Print service summary
            print(f"\n📦 Service: {service.name}")
            print(f"   Version: {service.version}")
            print(f"   Address: {service.endpoint.address}")
            print(f"   Status: {service_json['status']}")
            print(f"   Namespaces: {len(service.namespaces)}")
            
            # Count schedulable methods and show enhanced metadata
            schedulable_count = 0
            total_methods = 0
            for ns in service_json['namespaces']:
                for method in ns['methods']:
                    total_methods += 1
                    if method.get('x_scheduling', {}).get('enabled'):
                        schedulable_count += 1
                        if 'x_scheduling' in method:
                            sched = method['x_scheduling']
                            profile = sched.get('profile', {})
                            print(f"     ⏰ {method['name']} - {profile.get('category', 'unknown')} ({profile.get('business_impact', 'unknown')})")
                        else:
                            print(f"     ⏰ {method['name']} (schedulable)")
            
            print(f"   📊 Total methods: {total_methods}, Schedulable: {schedulable_count}")
            
            services.append(service_json)
        
        # Close channel
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
def get_service(service_name):
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

@app.route('/api/services/<service_name>/call', methods=['POST'])
def call_service_method(service_name):
    """Forward method call to service"""
    try:
        data = request.get_json()
        method_name = data.get('method')
        parameters = data.get('parameters', {})
        
        print(f"📞 Calling {service_name}.{method_name}")
        print(f"   Parameters: {json.dumps(parameters, indent=2)}")
        
        # TODO: Implement actual service calling
        # This would use the dynamic gRPC client to make the actual call
        
        return jsonify({
            "status": "success",
            "message": f"Method {method_name} called successfully",
            "result": {}
        })
        
    except Exception as e:
        print(f"❌ Error: {str(e)}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/api/schedule', methods=['POST', 'OPTIONS'])
def schedule_action():
    """Schedule an action via the scheduler service"""
    print("📌 Schedule endpoint called")
    
    if request.method == 'OPTIONS':
        # Handle CORS preflight
        response = jsonify({})
        response.headers.add('Access-Control-Allow-Origin', '*')
        response.headers.add('Access-Control-Allow-Headers', 'Content-Type')
        response.headers.add('Access-Control-Allow-Methods', 'POST')
        return response
    
    try:
        print("📌 Getting JSON data...")
        data = request.get_json()
        if not data:
            print("❌ No JSON data received")
            return jsonify({"error": "No data provided"}), 400
            
        service_name = data.get('service')
        method_name = data.get('method')
        schedule_date = data.get('date')
        schedule_time = data.get('time')
        service_address = data.get('service_address', '')
        parameters = data.get('parameters', {})
        
        print(f"📅 Scheduling: {service_name}.{method_name} at {schedule_date} {schedule_time}")
        print(f"   Parameters: {json.dumps(parameters, indent=2)}")
        
        # Call the scheduler service
        # First, find the scheduler service
        print("   🔍 Getting service discovery client...")
        list_stub, channel = get_service_discovery_client()
        print("   🔍 Creating find service stub...")
        find_stub = sd_grpc.get_service_serviceStub(channel)
        
        print("   🔍 Creating find request...")
        find_request = sd_pb2.get_service_request()
        find_request.service_name = "ifex_scheduler"
        
        print("   🔍 Finding scheduler service...")
        find_response = find_stub.get_service(find_request)
        
        if not find_response.service_info or not find_response.service_info.name:
            channel.close()
            return jsonify({
                "error": "Scheduler service not found"
            }), 404
        
        scheduler_service_info = find_response.service_info
        scheduler_address = scheduler_service_info.endpoint.address
        
        print(f"   📡 Found scheduler at: {scheduler_address}")
        
        # Now call the scheduler service to actually schedule the action
        print(f"   🔗 Connecting to scheduler at: {scheduler_address}")
        scheduler_channel = grpc.insecure_channel(scheduler_address)
        
        
        # Create scheduler client using new API
        sched_stub = sched_grpc.create_job_serviceStub(scheduler_channel)
        
        # Create job request using new structure
        sched_request = sched_pb2.create_job_request()
        
        # Build job_create_t structure - the field is "job" not "job_data"
        job = sched_request.job
        job.title = f"{service_name}.{method_name}"
        job.service = service_name
        job.method = method_name
        job.parameters = json.dumps(parameters)
        job.service_address = service_address
        
        # Convert date and time to ISO format
        # Combine date and time into ISO 8601 format with seconds
        # Ensure time has seconds (HH:MM becomes HH:MM:00)
        if len(schedule_time.split(':')) == 2:
            schedule_time += ':00'
        datetime_str = f"{schedule_date}T{schedule_time}"
        job.scheduled_time = datetime_str
        
        # Handle recurring/cron if specified
        recurring = data.get('recurring', 'once')
        if recurring != 'once':
            # Convert GUI recurring options to simple recurrence rules
            # The scheduler expects simple strings like "daily", "weekly", etc.
            job.recurrence_rule = recurring
        
        print(f"   📤 Calling scheduler.create_job...")
        
        # Call the scheduler
        sched_response = sched_stub.create_job(sched_request)
        
        scheduler_channel.close()
        channel.close()
        
        if sched_response.success:
            return jsonify({
                "message": sched_response.message if sched_response.message else "Job created successfully",
                "schedule_id": sched_response.job_id
            })
        else:
            return jsonify({
                "error": sched_response.message if sched_response.message else "Failed to create job"
            }), 400
        
    except Exception as e:
        import traceback
        print(f"❌ Scheduling error: {str(e)}")
        print(f"❌ Traceback: {traceback.format_exc()}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/api/jobs', methods=['GET'])
def get_jobs():
    """Get list of scheduled jobs from scheduler service"""
    try:
        print("📋 Fetching scheduled jobs...")
        
        # Find the scheduler service
        list_stub, channel = get_service_discovery_client()
        find_stub = sd_grpc.get_service_serviceStub(channel)
        
        find_request = sd_pb2.get_service_request()
        find_request.service_name = "ifex_scheduler"
        
        find_response = find_stub.get_service(find_request)
        
        if not find_response.service_info or not find_response.service_info.name:
            channel.close()
            return jsonify({
                "error": "Scheduler service not found"
            }), 404
        
        scheduler_service_info = find_response.service_info
        scheduler_address = scheduler_service_info.endpoint.address
        
        print(f"   📡 Found scheduler at: {scheduler_address}")
        
        # Connect to scheduler and get jobs
        scheduler_channel = grpc.insecure_channel(scheduler_address)
        
        
        # Create scheduler client
        sched_stub = sched_grpc.get_jobs_serviceStub(scheduler_channel)
        
        # Create get jobs request
        jobs_request = sched_pb2.get_jobs_request()
        
        print(f"   📤 Calling scheduler.get_jobs...")
        
        # Call the scheduler
        jobs_response = sched_stub.get_jobs(jobs_request)
        
        scheduler_channel.close()
        channel.close()
        
        # Check if response was successful
        if not jobs_response.success:
            return jsonify({
                "error": "Failed to fetch jobs from scheduler"
            }), 500
            
        # Convert jobs to the format expected by GUI
        jobs = []
        for job in jobs_response.jobs:
            # Map status enum to string
            status_map = {
                sched_pb2.PENDING: "PENDING",
                sched_pb2.RUNNING: "RUNNING",
                sched_pb2.COMPLETED: "COMPLETED",
                sched_pb2.FAILED: "FAILED",
                sched_pb2.CANCELLED: "CANCELLED"
            }
            
            jobs.append({
                "id": job.id,
                "service": job.service,
                "method": job.method,
                "scheduled_time": job.scheduled_time,
                "status": status_map.get(job.status, "UNKNOWN"),
                "recurring": bool(job.recurrence_rule),
                "recurrence_rule": job.recurrence_rule,
                "next_run": job.next_run_time if job.next_run_time else None,
                "last_run": job.executed_at if job.executed_at else None,
                "error_message": job.error_message if job.error_message else None
            })
        
        return jsonify({
            "jobs": jobs,
            "count": len(jobs)
        })
            
    except Exception as e:
        import traceback
        print(f"❌ Jobs fetch error: {str(e)}")
        print(f"❌ Traceback: {traceback.format_exc()}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/api/jobs/<job_id>', methods=['DELETE'])
def cancel_job(job_id):
    """Cancel/delete a scheduled job"""
    try:
        print(f"🗑️ Canceling job: {job_id}")
        
        # Find the scheduler service
        list_stub, channel = get_service_discovery_client()
        find_stub = sd_grpc.get_service_serviceStub(channel)
        
        find_request = sd_pb2.get_service_request()
        find_request.service_name = "ifex_scheduler"
        
        find_response = find_stub.get_service(find_request)
        
        if not find_response.service_info or not find_response.service_info.name:
            channel.close()
            return jsonify({
                "error": "Scheduler service not found"
            }), 404
        
        scheduler_service_info = find_response.service_info
        scheduler_address = scheduler_service_info.endpoint.address
        
        # Connect to scheduler
        scheduler_channel = grpc.insecure_channel(scheduler_address)
        
        # Create scheduler client for delete_job
        sched_stub = sched_grpc.delete_job_serviceStub(scheduler_channel)
        
        # Create delete request
        delete_request = sched_pb2.delete_job_request()
        delete_request.job_id = job_id
        
        print(f"   📤 Calling scheduler.delete_job...")
        
        # Call the scheduler
        delete_response = sched_stub.delete_job(delete_request)
        
        scheduler_channel.close()
        channel.close()
        
        if delete_response.success:
            return jsonify({
                "message": delete_response.message if delete_response.message else "Job deleted successfully"
            })
        else:
            return jsonify({
                "error": delete_response.message if delete_response.message else "Failed to delete job"
            }), 400
            
    except Exception as e:
        import traceback
        print(f"❌ Delete job error: {str(e)}")
        print(f"❌ Traceback: {traceback.format_exc()}")
        return jsonify({
            "error": str(e)
        }), 500

@app.route('/scheduler_live_gui.html', methods=['GET'])
def serve_gui():
    """Serve the GUI HTML file"""
    try:
        with open('scheduler_live_gui.html', 'r') as f:
            return f.read(), 200, {'Content-Type': 'text/html'}
    except FileNotFoundError:
        return "GUI file not found", 404

@app.route('/scheduler_calendar_gui.html', methods=['GET'])
def serve_calendar_gui():
    """Serve the Calendar GUI HTML file"""
    try:
        with open('scheduler_calendar_gui.html', 'r') as f:
            return f.read(), 200, {'Content-Type': 'text/html'}
    except FileNotFoundError:
        return "Calendar GUI file not found", 404

@app.route('/health', methods=['GET'])
@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({"status": "healthy", "service": "service-discovery-proxy"})

if __name__ == '__main__':
    port = int(os.getenv('PROXY_PORT', '5001'))
    print(f"""
    🌐 IFEX Scheduler REST Proxy
    ================================
    Service Discovery: {SERVICE_DISCOVERY_HOST}:{SERVICE_DISCOVERY_PORT}
    REST API Port: {port}
    
    Endpoints:
    - GET  /api/services           - List all services
    - GET  /api/services/<name>    - Get service details
    - POST /api/services/<name>/call - Call service method
    - POST /api/schedule          - Schedule an action
    - GET  /api/jobs              - List scheduled jobs
    - DELETE /api/jobs/<id>       - Delete a scheduled job
    - GET  /health                - Health check
    
    Starting Flask server...
    """)
    
    app.run(host='0.0.0.0', port=port, debug=True)