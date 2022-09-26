from multiprocessing import Process, Queue
import datetime
import paho.mqtt.client as mqtt
import subprocess
import os


counter = 0

def on_trigger_event(client, userdata, message):

    message_str = message.payload.decode('utf-8')

    if message_str.lower() == 'true':
        print('Starting recording')

        counter += 1

        os.popen(f'arecord -D mic_boost -f S32_LE -c2 -r48000 --duration=10 rec-{counter}.wav')
            #subprocess.run(['arecord', '-D mic_boost -f S32_LE -c2 -r48000 --duration=10 test.wav'], stdout=subprocess.DEVNULL)
    else:
        #TODO : Wait 5 seconds then cut a recording
        pass


def on_connect(client, userdata, flags, rc):
    print('Connected')

    client.subscribe('/sensors/feeders/2/trigger', qos=0)

def main():
    global camera_threads

    mqtt_client = mqtt.Client()

    USERNAME = 'sensor_writer'
    PASSWORD = 'fln0eFi79yhK'

    mqtt_client.username_pw_set(USERNAME, PASSWORD)
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_trigger_event

    mqtt_client.connect('littlerascal', port=1883)

    mqtt_client.loop_forever()
    
    print('Complete')

if __name__ == '__main__':
    print('Starting')
    main()