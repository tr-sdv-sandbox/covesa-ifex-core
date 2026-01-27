#!/usr/bin/env python3
"""
IFEX V2 Scheduler Dashboard API

Simplified Flask API for the in-memory cloud scheduler service.
No PostgreSQL required - connects directly to cloud services via gRPC.
"""

import os
import sys
import json
import uuid
import time
from datetime import datetime, timezone
from flask import Flask, jsonify, request, send_file
from flask_cors import CORS
import grpc
import yaml

# Add proto_gen to path
sys.path.insert(0, os.path.dirname(__file__))
from proto_gen import cloud_scheduler_service_pb2 as scheduler_pb2
from proto_gen import cloud_scheduler_service_pb2_grpc as scheduler_grpc
from proto_gen import cloud_scheduler_sync_bridge_pb2 as sync_bridge_pb2
from proto_gen import cloud_scheduler_sync_bridge_pb2_grpc as sync_bridge_grpc
from proto_gen import scheduler_types_pb2 as scheduler_types
from proto_gen import cloud_backend_transport_service_pb2 as transport_pb2
from proto_gen import cloud_backend_transport_service_pb2_grpc as transport_grpc
from proto_gen import cloud_discovery_service_pb2 as discovery_pb2
from proto_gen import cloud_discovery_service_pb2_grpc as discovery_grpc

app = Flask(__name__)
CORS(app)

# Configuration from environment
SCHEDULER_HOST = os.getenv('SCHEDULER_HOST', 'localhost')
SCHEDULER_PORT = int(os.getenv('SCHEDULER_PORT', '50102'))
SYNC_BRIDGE_HOST = os.getenv('SYNC_BRIDGE_HOST', 'localhost')
SYNC_BRIDGE_PORT = int(os.getenv('SYNC_BRIDGE_PORT', '50103'))
TRANSPORT_HOST = os.getenv('TRANSPORT_HOST', 'localhost')
TRANSPORT_PORT = int(os.getenv('TRANSPORT_PORT', '50100'))
DISCOVERY_HOST = os.getenv('DISCOVERY_HOST', 'localhost')
DISCOVERY_PORT = int(os.getenv('DISCOVERY_PORT', '50101'))

# Status mappings (from scheduler_types.proto enums)
JOB_STATUS_NAMES = {
    0: 'pending',      # JOB_STATUS_PENDING
    1: 'running',      # JOB_STATUS_RUNNING
    2: 'completed',    # JOB_STATUS_COMPLETED
    3: 'failed',       # JOB_STATUS_FAILED
    4: 'cancelled',    # JOB_STATUS_CANCELLED
}

SYNC_STATE_NAMES = {
    0: 'pending',      # SYNC_PENDING
    1: 'synced',       # SYNC_SYNCED
}

AUTHORITY_NAMES = {
    0: 'cloud',        # AUTHORITY_CLOUD
    1: 'vehicle',      # AUTHORITY_VEHICLE
}


def get_scheduler_channel():
    """Create gRPC channel to scheduler service."""
    return grpc.insecure_channel(f'{SCHEDULER_HOST}:{SCHEDULER_PORT}')


def get_sync_bridge_channel():
    """Create gRPC channel to scheduler sync bridge."""
    return grpc.insecure_channel(f'{SYNC_BRIDGE_HOST}:{SYNC_BRIDGE_PORT}')


def get_transport_channel():
    """Create gRPC channel to cloud backend transport service."""
    return grpc.insecure_channel(f'{TRANSPORT_HOST}:{TRANSPORT_PORT}')


def get_discovery_channel():
    """Create gRPC channel to cloud discovery service."""
    return grpc.insecure_channel(f'{DISCOVERY_HOST}:{DISCOVERY_PORT}')


def epoch_ms_to_iso8601(epoch_ms):
    """Convert epoch milliseconds to ISO8601 string."""
    if not epoch_ms:
        return None
    dt = datetime.fromtimestamp(epoch_ms / 1000, tz=timezone.utc)
    return dt.strftime('%Y-%m-%dT%H:%M:%SZ')


def iso8601_to_epoch_ms(iso_str):
    """Convert ISO8601 string to epoch milliseconds."""
    if not iso_str:
        return 0
    try:
        dt = datetime.fromisoformat(iso_str.replace('Z', '+00:00'))
        return int(dt.timestamp() * 1000)
    except (ValueError, AttributeError):
        return 0


def job_to_dict(job):
    """Convert protobuf job_t to dictionary."""
    return {
        'job_id': job.job_id,
        'vehicle_id': job.vehicle_id,
        'title': job.title,
        'service': job.service,
        'method': job.method,
        'parameters_json': job.parameters_json,
        'status': JOB_STATUS_NAMES.get(job.status, 'unknown'),
        'status_code': job.status,
        'scheduled_time': epoch_ms_to_iso8601(job.scheduled_time_ms),
        'scheduled_time_ms': job.scheduled_time_ms,
        'recurrence_rule': job.recurrence_rule,
        'end_time_ms': job.end_time_ms,
        'next_run_time': epoch_ms_to_iso8601(job.next_run_time_ms),
        'next_run_time_ms': job.next_run_time_ms,
        'last_executed': epoch_ms_to_iso8601(job.last_executed_ms),
        'last_executed_ms': job.last_executed_ms,
        'created_at': epoch_ms_to_iso8601(job.created_at_ms),
        'updated_at': epoch_ms_to_iso8601(job.updated_at_ms),
        'created_by': job.created_by,
        'paused': job.paused,
        'deleted': job.deleted,
        'authority': AUTHORITY_NAMES.get(job.authority, 'cloud'),
        'cloud_seq': job.cloud_seq,
        'vehicle_seq': job.vehicle_seq,
        'sync_state': SYNC_STATE_NAMES.get(job.sync_state, 'pending'),
        'sync_state_code': job.sync_state,
    }


def execution_to_dict(execution):
    """Convert protobuf execution_record_t to dictionary."""
    return {
        'execution_id': execution.execution_id,
        'job_id': execution.job_id,
        'status': JOB_STATUS_NAMES.get(execution.status, 'unknown'),
        'status_code': execution.status,
        'executed_at': epoch_ms_to_iso8601(execution.executed_at_ms),
        'executed_at_ms': execution.executed_at_ms,
        'duration_ms': execution.duration_ms,
        'result_json': execution.result_json,
        'error_message': execution.error_message,
    }


# =============================================================================
# Dashboard HTML
# =============================================================================

@app.route('/')
def index():
    """Serve the dashboard HTML."""
    return send_file('scheduler_dashboard.html')


# =============================================================================
# Health Check
# =============================================================================

@app.route('/api/health')
def health():
    """Check health of cloud services."""
    # Check scheduler
    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.healthy_serviceStub(channel)
            response = stub.healthy(scheduler_pb2.healthy_request())
            scheduler_healthy = response.is_healthy
    except Exception as e:
        scheduler_healthy = False

    # Check transport
    try:
        with get_transport_channel() as channel:
            stub = transport_grpc.healthy_serviceStub(channel)
            response = stub.healthy(transport_pb2.healthy_request())
            transport_healthy = response.is_healthy
    except Exception as e:
        transport_healthy = False

    # Check discovery
    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.healthy_serviceStub(channel)
            response = stub.healthy(discovery_pb2.healthy_request())
            discovery_healthy = response.is_healthy
    except Exception as e:
        discovery_healthy = False

    all_healthy = scheduler_healthy and transport_healthy and discovery_healthy

    return jsonify({
        'status': 'healthy' if all_healthy else 'degraded',
        'services': {
            'scheduler': {
                'healthy': scheduler_healthy,
                'endpoint': f'{SCHEDULER_HOST}:{SCHEDULER_PORT}'
            },
            'transport': {
                'healthy': transport_healthy,
                'endpoint': f'{TRANSPORT_HOST}:{TRANSPORT_PORT}'
            },
            'discovery': {
                'healthy': discovery_healthy,
                'endpoint': f'{DISCOVERY_HOST}:{DISCOVERY_PORT}'
            }
        }
    })


# =============================================================================
# Stats
# =============================================================================

@app.route('/api/stats')
def stats():
    """Get fleet-wide job statistics (computed from list_jobs)."""
    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_jobs_serviceStub(channel)
            response = stub.list_jobs(
                scheduler_pb2.list_jobs_request(
                    filter=scheduler_pb2.list_jobs_filter_t(page_size=1000)
                )
            )

            # Compute stats from jobs
            jobs = response.result.jobs
            vehicles = set()
            by_service = {}

            for job in jobs:
                vehicles.add(job.vehicle_id)
                key = (job.service, job.method)
                if key not in by_service:
                    by_service[key] = {
                        'service': job.service,
                        'method': job.method,
                        'total_jobs': 0,
                        'pending': 0,
                        'running': 0,
                        'completed': 0,
                        'failed': 0,
                        'recurring': 0
                    }
                by_service[key]['total_jobs'] += 1
                status = job.status
                if status == 0:  # JOB_STATUS_PENDING
                    by_service[key]['pending'] += 1
                elif status == 1:  # JOB_STATUS_RUNNING
                    by_service[key]['running'] += 1
                elif status == 2:  # JOB_STATUS_COMPLETED
                    by_service[key]['completed'] += 1
                elif status == 3:  # JOB_STATUS_FAILED
                    by_service[key]['failed'] += 1
                if job.recurrence_rule:
                    by_service[key]['recurring'] += 1

            return jsonify({
                'total_jobs': len(jobs),
                'total_vehicles': len(vehicles),
                'by_service_method': list(by_service.values())
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


# =============================================================================
# Vehicles (from cloud backend transport)
# =============================================================================

# Vehicle status mapping from transport proto
VEHICLE_STATUS_NAMES = {
    0: 'unknown',
    1: 'online',
    2: 'offline'
}


@app.route('/api/vehicles')
def list_vehicles():
    """List connected vehicles from cloud backend transport."""
    try:
        # Get vehicles from transport service
        vehicles_data = {}
        with get_transport_channel() as channel:
            stub = transport_grpc.list_vehicles_serviceStub(channel)
            response = stub.list_vehicles(transport_pb2.list_vehicles_request())

            for v in response.result.vehicles:
                vehicles_data[v.vehicle_id] = {
                    'vehicle_id': v.vehicle_id,
                    'status': VEHICLE_STATUS_NAMES.get(v.status, 'unknown'),
                    'is_online': v.status == 1,  # ONLINE
                    'last_seen_ms': v.last_seen_ms,
                    'last_seen': epoch_ms_to_iso8601(v.last_seen_ms),
                    'job_count': 0,
                    'pending_count': 0,
                    'running_count': 0
                }

        # Enrich with job counts from scheduler
        try:
            with get_scheduler_channel() as channel:
                stub = scheduler_grpc.list_jobs_serviceStub(channel)
                response = stub.list_jobs(
                    scheduler_pb2.list_jobs_request(
                        filter=scheduler_pb2.list_jobs_filter_t(
                            page_size=1000
                        )
                    )
                )

                for job in response.result.jobs:
                    vid = job.vehicle_id
                    if vid in vehicles_data:
                        vehicles_data[vid]['job_count'] += 1
                        if job.status == 1:  # pending
                            vehicles_data[vid]['pending_count'] += 1
                        elif job.status == 3:  # running
                            vehicles_data[vid]['running_count'] += 1
                    else:
                        # Vehicle has jobs but isn't connected - add it
                        vehicles_data[vid] = {
                            'vehicle_id': vid,
                            'status': 'unknown',
                            'is_online': False,
                            'last_seen_ms': 0,
                            'last_seen': None,
                            'job_count': 1,
                            'pending_count': 1 if job.status == 1 else 0,
                            'running_count': 1 if job.status == 3 else 0
                        }
        except Exception as e:
            # Scheduler might not be available - that's OK
            pass

        vehicles = sorted(vehicles_data.values(), key=lambda v: v['vehicle_id'])

        return jsonify({
            'vehicles': vehicles,
            'total': len(vehicles)
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


# =============================================================================
# Jobs / Calendar
# =============================================================================

@app.route('/api/jobs')
def list_jobs():
    """List all jobs with optional filtering."""
    vehicle_id = request.args.get('vehicle_id')
    service_filter = request.args.get('service')
    status_filter = request.args.get('status')
    page_size = int(request.args.get('page_size', 100))

    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_jobs_serviceStub(channel)

            filter_obj = scheduler_pb2.list_jobs_filter_t(
                page_size=page_size
            )
            if vehicle_id:
                filter_obj.vehicle_id_filter = vehicle_id
            if service_filter:
                filter_obj.service_filter = service_filter

            response = stub.list_jobs(
                scheduler_pb2.list_jobs_request(filter=filter_obj)
            )

            jobs = [job_to_dict(job) for job in response.result.jobs]

            return jsonify({
                'jobs': jobs,
                'total': response.result.total_count
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/calendar/<vehicle_id>')
def get_vehicle_calendar(vehicle_id):
    """Get scheduled jobs for a vehicle (calendar view)."""
    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_jobs_serviceStub(channel)

            response = stub.list_jobs(
                scheduler_pb2.list_jobs_request(
                    filter=scheduler_pb2.list_jobs_filter_t(
                        vehicle_id_filter=vehicle_id,
                        page_size=500
                    )
                )
            )

            jobs = [job_to_dict(job) for job in response.result.jobs]

            return jsonify({
                'vehicle_id': vehicle_id,
                'calendar': jobs,
                'total': len(jobs)
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/calendar/<vehicle_id>/hash')
def get_calendar_hash(vehicle_id):
    """Get hash of calendar state for efficient change detection.

    Poll this endpoint to check if calendar has changed.
    Only fetch full calendar when hash changes.

    Query params:
    - start_time_ms: Filter jobs with scheduled_time >= this value
    - end_time_ms: Filter jobs with scheduled_time <= this value

    Returns:
    {
        "state_hash": 12345678901234,
        "job_count": 5
    }
    """
    start_time_ms = request.args.get('start_time_ms', type=int, default=0)
    end_time_ms = request.args.get('end_time_ms', type=int, default=0)

    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_jobs_hash_serviceStub(channel)

            filter_obj = scheduler_pb2.list_jobs_filter_t(
                vehicle_id_filter=vehicle_id
            )
            if start_time_ms:
                filter_obj.start_time_ms = start_time_ms
            if end_time_ms:
                filter_obj.end_time_ms = end_time_ms

            response = stub.list_jobs_hash(
                scheduler_pb2.list_jobs_hash_request(filter=filter_obj)
            )

            result = response.result
            return jsonify({
                'vehicle_id': vehicle_id,
                'state_hash': str(result.state_hash),  # String to preserve precision in JS
                'job_count': result.job_count
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/calendar/<vehicle_id>', methods=['POST'])
def create_job(vehicle_id):
    """Create a new scheduled job for a vehicle."""
    data = request.json

    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.create_job_serviceStub(channel)

            req = scheduler_pb2.create_job_request_t(
                vehicle_id=vehicle_id,
                title=data.get('title', 'Untitled Job'),
                service=data.get('service', ''),
                method=data.get('method', ''),
                parameters_json=data.get('parameters_json', '{}'),
                scheduled_time_ms=iso8601_to_epoch_ms(data.get('scheduled_time')) or data.get('scheduled_time_ms', 0),
                recurrence_rule=data.get('recurrence_rule', ''),
                end_time_ms=iso8601_to_epoch_ms(data.get('end_time')) or data.get('end_time_ms', 0),
                created_by=data.get('created_by', 'dashboard')
            )

            response = stub.create_job(
                scheduler_pb2.create_job_request(request=req)
            )

            result = response.result
            if result.success:
                return jsonify({
                    'success': True,
                    'job_id': result.job_id
                })
            else:
                return jsonify({
                    'success': False,
                    'error': result.error_message
                }), 400
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/api/calendar/<vehicle_id>/<job_id>', methods=['DELETE'])
def delete_job(vehicle_id, job_id):
    """Delete a scheduled job."""
    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.delete_job_serviceStub(channel)

            response = stub.delete_job(
                scheduler_pb2.delete_job_request(
                    vehicle_id=vehicle_id,
                    job_id=job_id
                )
            )

            result = response.result
            return jsonify({
                'success': result.success,
                'error': result.error_message if not result.success else None
            })
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


@app.route('/api/calendar/<vehicle_id>/command', methods=['POST'])
def send_job_command(vehicle_id):
    """Send a command to a job (pause, resume, trigger, update)."""
    data = request.json
    command = data.get('command')
    job_id = data.get('job_id')

    if not command or not job_id:
        return jsonify({'success': False, 'error': 'Missing command or job_id'}), 400

    try:
        with get_scheduler_channel() as channel:
            if command == 'pause':
                stub = scheduler_grpc.pause_job_serviceStub(channel)
                response = stub.pause_job(
                    scheduler_pb2.pause_job_request(vehicle_id=vehicle_id, job_id=job_id)
                )
            elif command == 'resume':
                stub = scheduler_grpc.resume_job_serviceStub(channel)
                response = stub.resume_job(
                    scheduler_pb2.resume_job_request(vehicle_id=vehicle_id, job_id=job_id)
                )
            elif command == 'trigger':
                # Trigger goes through sync bridge, not scheduler
                with get_sync_bridge_channel() as bridge_channel:
                    stub = sync_bridge_grpc.trigger_job_serviceStub(bridge_channel)
                    response = stub.trigger_job(
                        sync_bridge_pb2.trigger_job_request(vehicle_id=vehicle_id, job_id=job_id)
                    )
                    return jsonify({
                        'success': response.sent,
                        'error': response.error_message if not response.sent else None
                    })
            elif command == 'update':
                stub = scheduler_grpc.update_job_serviceStub(channel)
                req = scheduler_pb2.update_job_request_t(
                    vehicle_id=vehicle_id,
                    job_id=job_id,
                    title=data.get('title', ''),
                    scheduled_time_ms=iso8601_to_epoch_ms(data.get('scheduled_time')) or data.get('scheduled_time_ms', 0),
                    recurrence_rule=data.get('recurrence_rule', ''),
                    parameters_json=data.get('parameters_json', '')
                )
                response = stub.update_job(
                    scheduler_pb2.update_job_request(request=req)
                )
            else:
                return jsonify({'success': False, 'error': f'Unknown command: {command}'}), 400

            result = response.result
            return jsonify({
                'success': result.success,
                'error': result.error_message if not result.success else None
            })
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)}), 500


# =============================================================================
# Job Executions
# =============================================================================

@app.route('/api/jobs/<vehicle_id>/<job_id>/executions')
def get_job_executions(vehicle_id, job_id):
    """Get execution history for a job."""
    limit = int(request.args.get('limit', 50))

    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_executions_serviceStub(channel)

            response = stub.list_executions(
                scheduler_pb2.list_executions_request(
                    filter=scheduler_pb2.list_executions_filter_t(
                        vehicle_id=vehicle_id,
                        job_id=job_id,
                        limit=limit
                    )
                )
            )

            result = response.result
            executions = [execution_to_dict(e) for e in result.executions]

            return jsonify({
                'vehicle_id': vehicle_id,
                'job_id': job_id,
                'executions': executions,
                'total': result.total_count
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/executions')
def list_all_executions():
    """List recent executions across all jobs."""
    limit = int(request.args.get('limit', 100))

    try:
        with get_scheduler_channel() as channel:
            stub = scheduler_grpc.list_executions_serviceStub(channel)

            response = stub.list_executions(
                scheduler_pb2.list_executions_request(
                    filter=scheduler_pb2.list_executions_filter_t(
                        limit=limit
                    )
                )
            )

            result = response.result
            executions = [execution_to_dict(e) for e in result.executions]

            return jsonify({
                'executions': executions,
                'total': result.total_count
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


# =============================================================================
# Discovery / Services
# =============================================================================

@app.route('/api/services')
def list_services():
    """List all discovered services across the fleet."""
    service_filter = request.args.get('service')
    vehicle_id = request.args.get('vehicle_id')

    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.find_services_serviceStub(channel)

            filter_obj = discovery_pb2.service_filter_t()
            if service_filter:
                filter_obj.service_name = service_filter
            if vehicle_id:
                filter_obj.vehicle_id = vehicle_id

            response = stub.find_services(
                discovery_pb2.find_services_request(filter=filter_obj)
            )

            services = []
            for svc in response.services:
                services.append({
                    'vehicle_id': svc.vehicle_id,
                    'name': svc.name,
                    'version': svc.version,
                    'schema_hash': svc.schema_hash,
                    'status': discovery_pb2.service_status_t.Name(svc.status).lower(),
                    'last_heartbeat_ms': svc.last_heartbeat_ms
                })

            return jsonify({
                'services': services,
                'total': response.total_count
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/services/<vehicle_id>')
def get_vehicle_services(vehicle_id):
    """Get services registered on a specific vehicle."""
    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.get_vehicle_services_serviceStub(channel)

            response = stub.get_vehicle_services(
                discovery_pb2.get_vehicle_services_request(vehicle_id=vehicle_id)
            )

            services = []
            for svc in response.services:
                services.append({
                    'vehicle_id': svc.vehicle_id,
                    'name': svc.name,
                    'version': svc.version,
                    'schema_hash': svc.schema_hash,
                    'status': discovery_pb2.service_status_t.Name(svc.status).lower(),
                    'last_heartbeat_ms': svc.last_heartbeat_ms
                })

            sync_info = None
            if response.HasField('sync_info'):
                sync_info = {
                    'vehicle_id': response.sync_info.vehicle_id,
                    'sync_status': discovery_pb2.sync_status_t.Name(response.sync_info.sync_status).lower(),
                    'last_sync_ms': response.sync_info.last_sync_ms,
                    'service_count': response.sync_info.service_count
                }

            return jsonify({
                'vehicle_id': vehicle_id,
                'services': services,
                'sync_info': sync_info
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/fleet/capabilities')
def get_fleet_capabilities():
    """Get aggregated service capabilities across the fleet."""
    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.get_fleet_capabilities_serviceStub(channel)

            response = stub.get_fleet_capabilities(
                discovery_pb2.get_fleet_capabilities_request()
            )

            capabilities = []
            for cap in response.capabilities:
                capabilities.append({
                    'service_name': cap.service_name,
                    'version': cap.version,
                    'vehicle_count': cap.vehicle_count,
                    'available_count': cap.available_count,
                    'methods': list(cap.methods)
                })

            return jsonify({
                'capabilities': capabilities,
                'total': len(capabilities)
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/schemas')
def list_schemas():
    """List all known IFEX schemas."""
    service_filter = request.args.get('service')

    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.list_schemas_serviceStub(channel)

            response = stub.list_schemas(
                discovery_pb2.list_schemas_request(
                    service_name_filter=service_filter or ''
                )
            )

            schemas = []
            for schema in response.schemas:
                schemas.append({
                    'schema_hash': schema.schema_hash,
                    'service_name': schema.service_name,
                    'version': schema.version,
                    'first_seen_ms': schema.first_seen_ms,
                    'vehicle_count': schema.vehicle_count
                })

            return jsonify({
                'schemas': schemas,
                'total': len(schemas)
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/schemas/<schema_hash>')
def get_schema(schema_hash):
    """Get full IFEX schema by hash."""
    try:
        with get_discovery_channel() as channel:
            stub = discovery_grpc.get_schema_serviceStub(channel)

            response = stub.get_schema(
                discovery_pb2.get_schema_request(schema_hash=schema_hash)
            )

            if not response.found:
                return jsonify({'error': 'Schema not found'}), 404

            schema = response.schema

            # Parse IFEX YAML to extract methods, structs, enums
            parsed = parse_ifex_yaml(schema.ifex_yaml)

            return jsonify({
                'schema_hash': schema.schema_hash,
                'service_name': schema.service_name,
                'version': schema.version,
                'ifex_yaml': schema.ifex_yaml,
                'first_seen_ms': schema.first_seen_ms,
                'vehicle_count': schema.vehicle_count,
                'methods': parsed.get('methods', []),
                'structs': parsed.get('structs', {}),
                'enums': parsed.get('enums', {})
            })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


def parse_ifex_yaml(ifex_yaml):
    """Parse IFEX YAML to extract methods, structs, enums."""
    try:
        data = yaml.safe_load(ifex_yaml)
        if not data:
            return {'methods': [], 'structs': {}, 'enums': {}}

        methods = []
        structs = {}
        enums = {}

        # Get namespaces - IFEX structure has namespaces containing methods directly
        namespaces = data.get('namespaces', [])
        for ns in namespaces:
            ns_name = ns.get('name', '')
            # Methods can be directly under namespace or under interface
            ns_methods = ns.get('methods', []) or ns.get('interface', {}).get('methods', [])

            # Parse methods
            for method in ns_methods:
                method_info = {
                    'name': method.get('name'),
                    'namespace': ns_name,
                    'description': method.get('description', ''),
                    'input': [],
                    'output': []
                }

                # Parse input parameters
                for inp in method.get('input', []):
                    param = {
                        'name': inp.get('name'),
                        'datatype': inp.get('datatype'),
                        'description': inp.get('description', '')
                    }
                    if 'default' in inp:
                        param['default'] = inp['default']
                    if 'range' in inp:
                        param['constraints'] = {
                            'min': inp['range'][0] if len(inp['range']) > 0 else None,
                            'max': inp['range'][1] if len(inp['range']) > 1 else None
                        }
                    method_info['input'].append(param)

                # Parse output
                for out in method.get('output', []):
                    method_info['output'].append({
                        'name': out.get('name'),
                        'datatype': out.get('datatype'),
                        'description': out.get('description', '')
                    })

                methods.append(method_info)

            # Parse structs - can be under namespace directly or as typedefs
            ns_structs = ns.get('structs', []) or ns.get('typedefs', []) or ns.get('interface', {}).get('typedefs', [])
            for struct_def in ns_structs:
                struct_name = struct_def.get('name')
                # Skip if it's a typedef that's not a struct
                if struct_def.get('datatype') and struct_def.get('datatype') != 'struct' and not struct_def.get('members'):
                    continue
                members = []
                for member in struct_def.get('members', []):
                    m = {
                        'name': member.get('name'),
                        'datatype': member.get('datatype'),
                        'description': member.get('description', '')
                    }
                    if 'default' in member:
                        m['default'] = member['default']
                    # Handle constraints (IFEX uses 'constraints' dict with min/max)
                    if 'constraints' in member:
                        m['constraints'] = {
                            'min': member['constraints'].get('min'),
                            'max': member['constraints'].get('max')
                        }
                    # Also handle legacy 'range' format
                    elif 'range' in member:
                        m['constraints'] = {
                            'min': member['range'][0] if len(member['range']) > 0 else None,
                            'max': member['range'][1] if len(member['range']) > 1 else None
                        }
                    members.append(m)
                if members:
                    structs[struct_name] = members

            # Parse enumerations - can be under namespace or interface
            ns_enums = ns.get('enumerations', []) or ns.get('interface', {}).get('enumerations', [])
            for enum in ns_enums:
                enum_name = enum.get('name')
                options = []
                for opt in enum.get('options', []):
                    opt_name = opt.get('name')
                    opt_value = opt.get('value', len(options))
                    options.append(f"{opt_name} ({opt_value})")
                enums[enum_name] = options

        return {'methods': methods, 'structs': structs, 'enums': enums}
    except Exception as e:
        print(f"Error parsing IFEX YAML: {e}")
        return {'methods': [], 'structs': {}, 'enums': {}}


# =============================================================================
# RPC Dispatch
# =============================================================================

@app.route('/api/rpc', methods=['POST'])
def execute_rpc():
    """Execute an RPC call on a vehicle service via cloud transport.

    Sends request to vehicle and waits for response with timeout.
    """
    data = request.json

    vehicle_id = data.get('vehicle_id')
    service_name = data.get('service_name')
    method_name = data.get('method_name')
    parameters = data.get('parameters', {})
    timeout_ms = data.get('timeout_ms', 10000)

    if not vehicle_id or not service_name or not method_name:
        return jsonify({
            'status': 'error',
            'error': 'Missing vehicle_id, service_name, or method_name'
        }), 400

    start_time = time.time()
    timeout_sec = timeout_ms / 1000.0

    try:
        from proto_gen import dispatcher_rpc_envelope_pb2 as rpc_pb2
        import threading

        correlation_id = str(uuid.uuid4())
        response_holder = {'response': None, 'error': None}
        response_event = threading.Event()

        # Build RPC request message
        rpc_request = rpc_pb2.rpc_request_t()
        rpc_request.correlation_id = correlation_id
        rpc_request.service_name = service_name
        rpc_request.method_name = method_name
        rpc_request.parameters_json = json.dumps(parameters)
        rpc_request.timeout_ms = timeout_ms
        rpc_request.request_timestamp_ms = int(time.time() * 1000)

        # Subscribe to responses in background thread
        def listen_for_response():
            try:
                with get_transport_channel() as channel:
                    stub = transport_grpc.on_vehicle_message_serviceStub(channel)

                    subscribe_req = transport_pb2.on_vehicle_message_subscribe_request()
                    subscribe_req.content_id = 200  # RPC content ID

                    # Set deadline for the subscription
                    context = grpc.insecure_channel(f'{TRANSPORT_HOST}:{TRANSPORT_PORT}')

                    for msg in stub.subscribe(subscribe_req):
                        if response_event.is_set():
                            break

                        # Parse response
                        try:
                            rpc_response = rpc_pb2.rpc_response_t()
                            rpc_response.ParseFromString(msg.message.payload)

                            if rpc_response.correlation_id == correlation_id:
                                response_holder['response'] = rpc_response
                                response_event.set()
                                break
                        except Exception as e:
                            pass  # Not our message or parse error

            except Exception as e:
                response_holder['error'] = str(e)
                response_event.set()

        # Start listener thread
        listener = threading.Thread(target=listen_for_response, daemon=True)
        listener.start()

        # Give listener a moment to subscribe
        time.sleep(0.1)

        # Send RPC request to vehicle
        with get_transport_channel() as channel:
            stub = transport_grpc.send_to_vehicle_serviceStub(channel)

            send_req = transport_pb2.send_to_vehicle_request()
            send_req.request.vehicle_id = vehicle_id
            send_req.request.content_id = 200  # RPC content ID
            send_req.request.payload = rpc_request.SerializeToString()
            send_req.request.persistence = transport_pb2.BEST_EFFORT

            response = stub.send_to_vehicle(send_req)

            if response.result.status != transport_pb2.OK:
                response_event.set()  # Stop listener
                elapsed_ms = int((time.time() - start_time) * 1000)
                status_name = transport_pb2.publish_status_t.Name(response.result.status)
                return jsonify({
                    'status': 'error',
                    'error': f'Failed to send to vehicle: {status_name}',
                    'duration_ms': elapsed_ms
                })

        # Wait for response
        got_response = response_event.wait(timeout=timeout_sec)
        elapsed_ms = int((time.time() - start_time) * 1000)

        if not got_response:
            return jsonify({
                'status': 'timeout',
                'error': f'No response from vehicle within {timeout_ms}ms',
                'correlation_id': correlation_id,
                'duration_ms': elapsed_ms
            })

        if response_holder['error']:
            return jsonify({
                'status': 'error',
                'error': response_holder['error'],
                'duration_ms': elapsed_ms
            })

        rpc_response = response_holder['response']

        if rpc_response.status == rpc_pb2.SUCCESS:
            result = None
            if rpc_response.result_json:
                try:
                    result = json.loads(rpc_response.result_json)
                except:
                    result = rpc_response.result_json

            return jsonify({
                'status': 'success',
                'result': result,
                'duration_ms': elapsed_ms,
                'vehicle_duration_ms': rpc_response.duration_ms
            })
        else:
            status_name = rpc_pb2.rpc_status_t.Name(rpc_response.status)
            return jsonify({
                'status': 'failed',
                'error': rpc_response.error_message or status_name,
                'rpc_status': status_name,
                'duration_ms': elapsed_ms
            })

    except Exception as e:
        elapsed_ms = int((time.time() - start_time) * 1000)
        return jsonify({
            'status': 'error',
            'error': str(e),
            'duration_ms': elapsed_ms
        }), 500


# =============================================================================
# Main
# =============================================================================

if __name__ == '__main__':
    port = int(os.getenv('DASHBOARD_PORT', '8080'))
    debug = os.getenv('DEBUG', 'false').lower() == 'true'

    print(f"IFEX V2 Scheduler Dashboard")
    print(f"  Dashboard: http://localhost:{port}")
    print(f"  Scheduler: {SCHEDULER_HOST}:{SCHEDULER_PORT}")
    print(f"  Transport: {TRANSPORT_HOST}:{TRANSPORT_PORT}")
    print(f"  Discovery: {DISCOVERY_HOST}:{DISCOVERY_PORT}")
    print()

    app.run(host='0.0.0.0', port=port, debug=debug)
