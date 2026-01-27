# IFEX V2 Scheduler Dashboard
#
# Build:
#   The start-simulation.sh script copies proto files before building.
#   docker build -t ifex-v2-dashboard:latest .
#
# Run:
#   docker run -p 8080:8080 -e SCHEDULER_HOST=host.docker.internal ifex-v2-dashboard:latest

FROM python:3.11-slim

WORKDIR /app

# Install dependencies (including grpcio-tools for proto generation)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt grpcio-tools

# Copy proto files (copied by start-simulation.sh before docker build)
COPY protos/ ./protos/

# Generate Python proto stubs
RUN mkdir -p proto_gen && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/cloud-backend-transport-service.proto && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/cloud-discovery-service.proto && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/common/scheduler-types.proto && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/cloud-scheduler-service.proto && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/cloud-scheduler-sync-bridge.proto && \
    python3 -m grpc_tools.protoc \
        -I./protos \
        --python_out=proto_gen \
        --grpc_python_out=proto_gen \
        ./protos/dispatcher-rpc-envelope.proto && \
    # Move common/*.py to proto_gen root (flatten structure)
    mv proto_gen/common/*.py proto_gen/ && \
    rmdir proto_gen/common && \
    touch proto_gen/__init__.py && \
    # Fix imports in generated files (change absolute to relative imports)
    for f in proto_gen/*_pb2_grpc.py proto_gen/*_pb2.py; do \
        sed -i 's/^import \([a-z_]*_pb2\) as/from . import \1 as/' "$f"; \
        sed -i 's/^from common import/from . import/' "$f"; \
    done

# Copy application
COPY scheduler_api.py .
COPY scheduler_dashboard.html .

# Environment defaults
ENV SCHEDULER_HOST=localhost
ENV SCHEDULER_PORT=50102
ENV SYNC_BRIDGE_HOST=localhost
ENV SYNC_BRIDGE_PORT=50103
ENV TRANSPORT_HOST=localhost
ENV TRANSPORT_PORT=50100
ENV DISCOVERY_HOST=localhost
ENV DISCOVERY_PORT=50101
ENV DASHBOARD_PORT=8080

EXPOSE 8080

CMD ["python3", "scheduler_api.py"]
