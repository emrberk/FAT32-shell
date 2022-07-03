all:
	g++ *.cpp -o hw && ./hw /home/bs2019/e2380590/example1.img 
debug:
	g++ -g *.cpp -o hw && gdb --args hw /home/bs2019/e2380590/example1.img
clean:
	rm -f ~/example1.img && cp ~/Desktop/example1.img ~
check:
	fsck.vfat -vn ~/example1.img
unmount:
	fusermount -u ~/root1 && rm -rf ~/root1
mount:
	mkdir ../root1 && chmod 777 ../root1 && fusefat -o rw+ -o umask=770 ../example1.img ../root1
