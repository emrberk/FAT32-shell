all:
	g++ *.cpp -o hw && ./hw /home/bs2019/e2380590/example3.img 
debug:
	g++ -g *.cpp -o hw && gdb --args hw /home/bs2019/e2380590/example3.img
