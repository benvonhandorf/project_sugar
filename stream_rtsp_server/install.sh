#!/bin/bash

export P1=$1
export HOSTNAME=`hostname`
export SERVICE=camera_stream
export DESCRIPTION="Camera Stream"

#User that should run the service
export USER=$SERVICE
#Group that should run the service
export GROUP=$USER

#Directory in the user's home to install this service.  Working directory will be set to this.
DIRECTORY=mediamtx

#Command for this service.  Relative to the directory.  
export EXEC="mediamtx"

#If REQUIRED_GROUPS aren't present on the machine they are ignored
REQUIRED_GROUPS=("gpio" "i2c" "video")

ARCH=`uname --machine`

function ensure_group() {
    local REQUIRED_GROUP=$1
    local USER=$2

    if getent group $REQUIRED_GROUP &>/dev/null; then
        echo "Group $REQUIRED_GROUP exists, adding user"
        sudo usermod -aG $REQUIRED_GROUP $USER
    fi
}

if [ $ARCH == "aarch64" ]; then
    echo "Configuring for Jetson"
    RELEASE="https://github.com/bluenviron/mediamtx/releases/download/v0.23.5/mediamtx_v0.23.5_linux_arm64v8.tar.gz"
    CONFIG="mediamtx_jetson.yml"

    chmod u+x run_stream_camera0.sh
    SUDOERS="sudoers_jetson"
else
    echo "Configuring for RPi"
    if [ $ARCH == "armv6l" ]; then
        RELEASE="https://github.com/bluenviron/mediamtx/releases/download/v0.23.5/mediamtx_v0.23.5_linux_armv6.tar.gz"
    else
        RELEASE="https://github.com/bluenviron/mediamtx/releases/download/v0.23.5/mediamtx_v0.23.5_linux_armv7.tar.gz"
    fi

    CONFIG="mediamtx_raspberrypi.yml"

    rm run_stream_camera0.sh

    SUDOERS="sudoers_raspberrypi"
fi

if [ ! -f "mediamtx" ]; then
    #Download the release
    wget $RELEASE
    tar -xvf mediamtx*.tar.gz
    rm mediamtx*.tar.gz
fi

#Replace the config file with the correct one
echo "Using $CONFIG for configuration"
mv $CONFIG mediamtx.yml

if [ ! -z "$SUDOERS" ]; then
    echo "Using $SUDOERS for sudoers"

    # Validate the damn sudoers file
    visudo -c -f $SUDOERS

    if [ $? -ne 0 ]; then
        echo "sudoers file is invalid.  Aborting"
        exit 1
    fi

    sudo cp $SUDOERS /etc/sudoers.d/$SERVICE
    sudo chown root:root /etc/sudoers.d/$SERVICE
    sudo chmod 440 /etc/sudoers.d/$SERVICE
fi

# Create the user and assign groups
if id "$USER" &>/dev/null; then
    echo "User $USER found"
else
    sudo useradd --system --shell /bin/false --create-home $USER
fi

for REQUIRED_GROUP in "${REQUIRED_GROUPS[@]}"; do
    ensure_group $REQUIRED_GROUP $USER
done

eval TARGET_DIRECTORY="~$USER/$DIRECTORY"
export TARGET_DIRECTORY

# Create the service file
echo "Creating service file"

cat service.template | envsubst > $SERVICE.service

sudo mkdir -p $TARGET_DIRECTORY
sudo cp * $TARGET_DIRECTORY
sudo chown --recursive $USER:$GROUP $TARGET_DIRECTORY

cd $TARGET_DIRECTORY

if [ -f "requirements.txt" ]; then
    sudo --user=$USER bash -c "python3 -m venv env ; . ./env/bin/activate ; pip3 install --no-cache-dir -r requirements.txt"
fi

sudo cp $SERVICE.service /etc/systemd/system/

sudo systemctl daemon-reload
sudo systemctl start $SERVICE
sudo systemctl status $SERVICE
sudo systemctl enable $SERVICE

# Wait a short time to ensure the service is stable
sleep 5

sudo systemctl status $SERVICE
