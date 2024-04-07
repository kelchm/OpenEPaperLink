# starting with an ancient version of Ubuntu
FROM --platform=linux/amd64 ubuntu:14.04

# Update the system and install necessary packages
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y \
    curl \
    xz-utils \
    libc6:i386 \
    make

# Download and extract the GNU Arm Embedded Toolchain (4.9-2015-q3-update)
RUN curl -SL https://launchpad.net/gcc-arm-embedded/4.9/4.9-2015-q3-update/+download/gcc-arm-none-eabi-4_9-2015q3-20150921-linux.tar.bz2 | tar -xjC /

# Add the toolchain binaries to the PATH
ENV PATH="/gcc-arm-none-eabi-4_9-2015q3/bin:${PATH}"
ENV ARMGCC_PATH="/gcc-arm-none-eabi-4_9-2015q3/bin/"

# Copy the builds deps of the workspace
COPY . /root/workspace/OpenEPaperLink

# Set the working directory
WORKDIR /root/workspace/OpenEPaperLink
