hosts=("picam2" "polecat" "picam03" "hqcam")

for host in "${hosts[@]}"; do
	rsync -u -v --progress pi@$host:~/video/* ./video/
done
