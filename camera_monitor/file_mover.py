from multiprocessing import Process, Queue
from frame_buffer import FrameBuffer
from stream_configuration import StreamConfiguration
from datetime import datetime
import common
from time import perf_counter
from fabric import Connection
from paramiko import Ed25519Key
from pathlib import Path

class FileMoverConfig:
    def __init__(self, server: str, username: str, target_path: str):
        self.server = server
        self.username = username
        self.target_path = target_path

class FileMover(Process):
    def __init__(self, configuration: FileMoverConfig, capture_complete_queue: Queue, move_complete_queue: Queue):
        Process.__init__(self)

        self.logger = common.get_logger('FileMover')

        self.configuration = configuration
        self.capture_complete_queue = capture_complete_queue
        self.move_complete_queue = move_complete_queue

    def initialize(self):
        self.logger = common.get_logger('FileMover')

        connect_kwargs = {
            'key_filename': (Path.home() / '.ssh' / 'fileserver_rsa').resolve().as_posix()
        }

        self.connection = Connection(self.configuration.server, 
                                     user=self.configuration.username,
                                     connect_kwargs=connect_kwargs)

    def process_capture_complete(self, capture_complete):
        #{'video': False, 'filename': filename}

        remote_suffix = 'video/' if capture_complete['video'] else 'snapshot/'

        remote_path = self.configuration.target_path + remote_suffix

        result = self.connection.put(capture_complete['filename'], remote=remote_path)

        if result:
            local_path = Path(capture_complete['filename'])
            full_remote_path = remote_path + local_path.name
            move_complete = { 'type': 'file_moved', 'video': capture_complete['video'], 'filename': full_remote_path}

            self.move_complete_queue.put(move_complete)

            local_path.unlink()
        else:
            self.logger.error(f'Unable to move file: {result}')

    def run(self):
        try: 
            self.logger.info(f'Running')

            self.initialize()

            while True:
                capture_complete = self.capture_complete_queue.get()

                self.logger.info(f'File Move Request: {capture_complete}')

                self.process_capture_complete(capture_complete)
        except Exception as e:
            self.logger.error(e)
