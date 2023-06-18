from multiprocessing import Process, Queue
import datetime
import paho.mqtt.client as mqtt
from capture_process import CaptureProcess

CAMERAS = [
        #{ 'name': 'picam2', 'url': 'rtsp://littlerascal:8554/picam2', 'frame_width': 640, 'frame_height': 480, 'frame_rate': 30 },
#        { 'name': 'picam04', 'url': 'rtsp://littlerascal:8554/picam04', 'frame_width': 1280, 'frame_height': 720, 'frame_rate': 30 },
        # { 'name': 'hqcam', 'url': 'rtsp://littlerascal:8554/hqcam', 'frame_width': 640, 'frame_height': 480, 'frame_rate': 90 },
        { 'name': 'polecat', 'url': 'rtsp://littlerascal:8554/polecat', 'frame_width': 640, 'frame_height': 480, 'frame_rate': 30 }
    ]

def create_thread_for_camera(camera):
    camera_queue = Queue()
    capture = CaptureProcess(camera_queue, camera['url'], camera['name'], camera['frame_width'], camera['frame_height'], camera['frame_rate'])

    capture.start()

    return (camera, capture, camera_queue)

camera_threads = None

def on_trigger_event(client, userdata, message):

    message_str = message.payload.decode('utf-8')
    print(message_str)

    if message_str.lower() == 'true':
        # Accept up to 100 seconds of recording without further messages
        new_timeout = datetime.datetime.now().timestamp() + 100
    else:
        new_timeout = datetime.datetime.now().timestamp() + 10

    print(f'{datetime.datetime.now().timestamp()}: Recording until {new_timeout} - {message_str}')

    if camera_threads is not None:
        for camera in camera_threads:
            print(f'Alerting {camera[0]["name"]}')
            camera[2].put(new_timeout)
    else:
        print('camera_threads is None')

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

    camera_threads = list(map(create_thread_for_camera, CAMERAS))

    mqtt_client.loop_forever()
    
    for camera in camera_threads:
        camera[1].join()

    print('Complete')

if __name__ == '__main__':
    print('Starting')
    main()
