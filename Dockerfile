# starting with a more modern version of Ubuntu
FROM --platform=linux/amd64 ubuntu:22.04

# Update the system and install necessary packages
RUN apt-get update && \
    apt-get install -y \
    git \
    curl \
    python3-pip \
    build-essential \
    gcc-arm-none-eabi

# Set python3 as the default python
RUN update-alternatives --install /usr/bin/python python /usr/bin/python3 1

# Install the necessary python packages
RUN pip3 install --upgrade platformio intelhex pillow

# TODO: this is a bit of a hack and should be improved
RUN curl -SL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp --create-dirs -o /usr/include/nlohmann/json.hpp

