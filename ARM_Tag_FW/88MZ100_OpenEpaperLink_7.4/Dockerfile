# starting with a more modern version of Ubuntu
FROM --platform=linux/amd64 ubuntu:20.04

# Update the system and install necessary packages
RUN apt-get update && \
    apt-get install -y \
    curl \
    python3-pip \
    build-essential \
    gcc-arm-none-eabi

# Install the necessary python packages (pillow for image processing)
RUN pip3 install --upgrade pillow

RUN curl -SL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp --create-dirs -o /usr/include/nlohmann/json.hpp

# Copy the builds deps of the workspace
COPY . /root/workspace/OpenEPaperLink
RUN chmod +x /root/workspace/OpenEPaperLink/ARM_Tag_FW/88MZ100_OpenEpaperLink_7.4/mkfsOEPL/compile_fs_tools.sh

# Set the working directory
WORKDIR /root/workspace/OpenEPaperLink
