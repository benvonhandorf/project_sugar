import logging
from multiprocessing_logging import install_mp_handler

def get_logger(name=None):
    logger = logging.getLogger(name=name)
    logger.setLevel(logging.DEBUG)

    if name is None and not logger.hasHandlers():
        stream_handler = logging.StreamHandler()
        stream_handler.setLevel(logging.DEBUG)

        formatter = logging.Formatter(fmt = '%(asctime)s %(name)s %(levelname)-8s %(message)s',
                                    datefmt='%Y-%m-%d %H:%M:%S')
        stream_handler.setFormatter(formatter)

        logger.addHandler(stream_handler)

        install_mp_handler(logger)

        logger.info('Installed multiprocessing logging handler')

    return logger