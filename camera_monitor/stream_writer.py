import cv2
from multiprocessing import Process, Queue
import logging
from frame_buffer import FrameBuffer
from enum import Enum
from stream_configuration import StreamConfiguration
from datetime import datetime
import common
from time import perf_counter

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
        timestamp_string = datetime.fromtimestamp(timestamp/1000).isoformat()

        filename = f'{self.stream_configuration.base_filename}-{timestamp_string}.mkv'

        self.logger.info(f'Writing to {filename}')

        fourcc = cv2.VideoWriter_fourcc(*'H264')
        self.video_writer = cv2.VideoWriter(filename, fourcc, self.stream_configuration.framerate, (self.stream_configuration.width, self.stream_configuration.height))

        if not self.video_writer.isOpened():
            self.logger.warn(f'Unable to open {filename} for writing - {self.stream_configuration}')

            self.stop_writer()
        else:
            self.state = StreamWriter.State.Started
            self.frames = 0
            self.timestamp_start = perf_counter()

    def stop_writer(self):
        if self.video_writer:
            self.video_writer.release()
            self.video_writer = None

        if self.state == StreamWriter.State.Started:
            timestamp_end = perf_counter()
            duration = timestamp_end - self.timestamp_start
            framerate = self.frames / duration

            self.logger.info(f'Wrote {self.frames} frames in {duration} seconds: {framerate} fps')

            self.state = StreamWriter.State.Stopped

    def write_available_frames(self):
        if self.state != StreamWriter.State.Started:
            return

        frame = self.frame_buffer.pop()

        while frame is not None:
            self.video_writer.write(frame)

            self.frames += 1
            
            frame = self.frame_buffer.pop()

        if self.frames % 100 == 0:
            self.logger.debug(f'Frames written: {self.frames}')

    def process_command(self, command):
        if command is not None:
            self.logger.debug(f'Received command: {command}')

            if command['value']:
                self.logger.debug(f'Beginning recording')

                self.start_writer(command['timestamp'])

                self.write_available_frames()

            else :
                self.logger.debug(f'Ending recording')

                self.stop_writer()
        
    def run(self):
        self.logger = common.get_logger('StreamWriter')

        self.logger.info(f'Running')

        self.state = StreamWriter.State.Stopped

        while True:

            if self.state == StreamWriter.State.Started:
                self.write_available_frames()

                if not self.queue.empty():
                    command = self.queue.get_nowait()

                    self.process_command(command)
            else:
                command = self.queue.get(True)

                self.process_command(command)

                    

            
                


        

