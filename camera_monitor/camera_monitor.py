from time import perf_counter
import frame_buffer
from stream_configuration import StreamConfiguration
from stream_reader import StreamReader
from stream_writer import StreamWriter
from snapshot_writer import SnapshotWriter
from file_mover import FileMover, FileMoverConfig
from multiprocessing import Queue, log_to_stderr
from controller import Controller
from mqtt_monitor import MqttConfiguration, MqttMonitor
import common
import os
import sys
import json
import signal

if __name__ == '__main__':
    logger = common.get_logger()

    logger.info('Starting')

    hostname = os.uname()[1]

    config_file = sys.argv[1] or '../camera_config.json'

    logger.info(f'Loading configuration from {config_file}')

    with open(config_file, 'r') as config_file:
        CONFIG = json.load(config_file)

    camera_host = CONFIG.get('camera_host') or os.uname()[1]
    camera_id = CONFIG['camera_id']

    mqtt_host = CONFIG['mqtt_host']
    mqtt_port = CONFIG['mqtt_port']
    mqtt_username = CONFIG['mqtt_username']
    mqtt_password = CONFIG['mqtt_password']
    feeder = CONFIG.get('feeder_id') or 'feeder01'

    root_topic = f'cameras/{camera_host}/{camera_id}/'
    camera_id = 'camera0'

    stream_configuration = StreamConfiguration(f'rtsp://{camera_host}:8554/{camera_id}', 'data', camera_host, CONFIG['camera_fps'], CONFIG['camera_width'], CONFIG['camera_height'])
        
    client_id = f'{hostname}_{camera_id}_monitor'
    feeder_id = 'feeder01'

    if not os.path.exists(stream_configuration.directory):
        logger.info(f'Creating directory {stream_configuration.directory}')
        os.makedirs(stream_configuration.directory)

    file_mover_config = FileMoverConfig('littlerascal', 'camera_files', f'storage/{feeder_id}/')

    frame_buffer_seconds = 5
    frame_buffer_count = stream_configuration.framerate * frame_buffer_seconds

    logger.info(f'Frame Buffer for {frame_buffer_count} frames.')

    frame_buffer_manager = frame_buffer.FrameBufferManager()
    frame_buffer_manager.start()

    frame_buffer = frame_buffer_manager.FrameBuffer(frame_buffer_count)

    control_queue = Queue()
    snapshot_queue = Queue()
    capture_complete_queue = Queue()
    file_moved_queue = Queue()

    stream_writer = StreamWriter(control_queue, capture_complete_queue, frame_buffer, stream_configuration)
    stream_writer.start()

    stream_reader = StreamReader(stream_configuration, frame_buffer)
    stream_reader.start()

    snapshot_writer = SnapshotWriter(snapshot_queue, capture_complete_queue, frame_buffer, stream_configuration)
    snapshot_writer.start()

    file_mover = FileMover(file_mover_config, capture_complete_queue, file_moved_queue)
    file_mover.start()

    controller = Controller(control_queue, snapshot_queue)

    mqtt_configuration = MqttConfiguration('littlerascal', 'camera', '8vSZa&#v7p1N', root_topic, client_id)

    mqtt_monitor = MqttMonitor(mqtt_configuration, controller, file_moved_queue)
    mqtt_monitor.start()

    def signal_handler(sig, frame):
        stream_writer.close()
        snapshot_writer.close()
        file_mover.close()
        mqtt_monitor.close()
        stream_reader.close()

        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.pause()

