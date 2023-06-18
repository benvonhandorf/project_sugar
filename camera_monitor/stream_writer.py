import cv2
from multiprocessing import Process, Queue
import logging
from frame_buffer import FrameBuffer
from enum import Enum
from stream_configuration import StreamConfiguration
from datetime import datetime
import common
import multiprocessing_logging

class StreamWriter(Process):
    class State(Enum):
        Initializing = 0
        Stopped = 1
        Started = 2

    def __init__(self, queue: Queue, frame_buffer: FrameBuffer, stream_configuration : StreamConfiguration):
        Process.__init__(self)

        self.queue = queue
        self.frame_buffer = frame_buffer
        self.stream_configuration = stream_configuration

        self.state = StreamWriter.State.Initializing
        self.writer = None

    def start_writer(self, timestamp):
        filename = f'{self.stream_configuration.base_filename}-{timestamp}.mkv'

        self.video_writer = cv2.VideoWriter(filename, 0, self.stream_configuration.framerate, (self.stream_configuration.width, self.stream_configuration.height))

    def stop_writer(self):
        self.video_writer.release()
        self.video_writer = None

    def write_backlog(self):
        self.logger.debug(f'Backlog: {self.frame_buffer.len()}')

        existing_frames = self.frame_buffer.snapshot()

        self.logger.debug(f'Writing backlog: {len(existing_frames)}')

        for frame in existing_frames:
            self.video_writer.write(frame)

        self.logger.debug(f'Backlog written: {len(existing_frames)}')
        
    def run(self):
        self.logger = common.get_logger('StreamWriter')

        self.logger.info(f'Running')

        self.state = StreamWriter.State.Stopped

        while True:

            if not self.queue.empty():
                command = self.queue.get_nowait()

                if command is not None:
                    self.logger.debug(f'Received command: {command}')

                    if command:
                        self.logger.debug(f'Beginning recording')

                        self.state = StreamWriter.State.Started

                        self.start_writer(datetime.now().isoformat())

                        self.write_backlog()

                    else :
                        self.logger.debug(f'Ending recording')

                        self.state = StreamWriter.State.Stopped

                        self.stop_writer()

            # TODO: Figure out the best way to get post-backlog frames into the file
                


        

