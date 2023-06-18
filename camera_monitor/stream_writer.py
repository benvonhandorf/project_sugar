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
        self.video_writer = None

    def start_writer(self, timestamp):
        filename = f'{self.stream_configuration.base_filename}-{timestamp}.mkv'

        self.logger.info(f'Writing to {filename}')

        self.video_writer = cv2.VideoWriter(filename, 0, self.stream_configuration.framerate, (self.stream_configuration.width, self.stream_configuration.height))

        if not self.video_writer.isOpened():
            self.logger.warn(f'Unable to open {filename} for writing')

            self.stop_writer()
        else:
            self.state = StreamWriter.State.Started

    def stop_writer(self):
        if self.video_writer:
            self.video_writer.release()
            self.video_writer = None

        self.state = StreamWriter.State.Stopped

    def write_available_frames(self):
        if self.state != StreamWriter.State.Started:
            return

        frames = 0

        frame = self.frame_buffer.pop()

        while frame is not None:
            self.video_writer.write(frame)

            frames += 1
            
            frame = self.frame_buffer.pop()

        if frames > 0:
            self.logger.debug(f'Backlog written: {frames}')
        
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

                        self.start_writer(datetime.now().isoformat())

                        self.write_available_frames()

                    else :
                        self.logger.debug(f'Ending recording')

                        self.stop_writer()

            if self.state == StreamWriter.State.Started:
                self.write_available_frames()
                


        

