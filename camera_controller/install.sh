#!/bin/bash

export P1=$1
export HOSTNAME=`hostname`
export SERVICE=camera_controller
export DESCRIPTION="Camera Controller"

#User that should run the service
export USER=camera_stream
#Group that should run the service
export GROUP=camera_stream

#Directory in the user's home to install this service.  Working directory will be set to this.
DIRECTORY=camera_controller

#Command for this service.  Relative to the directory.  
export EXEC="/usr/bin/python3 /home/camera_stream/camera_controller/camera_controller.py"

#If REQUIRED_GROUPS aren't present on the machine they are ignored
REQUIRED_GROUPS=()

ARCH=`uname --machine`

function ensure_group() {
    local REQUIRED_GROUP=$1
    local USER=$2

    if getent group $REQUIRED_GROUP &>/dev/null; then
        echo "Group $REQUIRED_GROUP exists, adding user"
        sudo usermod -aG $REQUIRED_GROUP $USER
    fi
}

cat sudoers.template | envsubst > $SERVICE.sudoers
SUDOERS=$SERVICE.sudoers

if [ ! -z "$SUDOERS" ]; then
    echo "Using $SUDOERS for sudoers"

    # Validate the damn sudoers file
    visudo -c -f $SUDOERS

    if [ $? -ne 0 ]; then
        echo "sudoers file is invalid.  Aborting."
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

if [[ $EXEC == *"/"* ]]; then
    export FULL_EXEC="$EXEC"
else
    export FULL_EXEC="$TARGET_DIRECTORY/$EXEC"
fi

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
