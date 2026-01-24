FROM python:3.12-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    git \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Clone the patched IFEX repository
RUN git clone -b array_handling https://github.com/skarlsson/ifex.git .

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Install the package in editable mode
RUN pip install -e .
 
# Create entrypoint script to handle different commands
RUN echo '#!/bin/bash\n\
if [ "$1" = "ifexgen" ]; then\n\
    shift\n\
    exec python -m packaging.entrypoints.generator "$@"\n\
elif [ "$1" = "ifexgen_dbus" ]; then\n\
    shift\n\
    exec python -m packaging.entrypoints.generator_dbus "$@"\n\
elif [ "$1" = "ifexconv_protobuf" ]; then\n\
    shift\n\
    exec python -m packaging.entrypoints.protobuf_ifex "$@"\n\
else\n\
    exec "$@"\n\
fi' > /entrypoint.sh && chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["ifexgen", "--help"]
