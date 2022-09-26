ssh pi@polecat "nohup /home/pi/start_stream.sh > /dev/null 2> /dev/null &"
ssh pi@picam2 "nohup /home/pi/start_stream.sh > /dev/null 2> /dev/null &"
ssh pi@hqcam "nohup /home/pi/start_stream.sh > /dev/null 2> /dev/null &"

