
default: s3

clean:
	rm *.o s3

s3: s3_main.o s3.o connect.o
	 g++ -L /root/apt-debian-sid/build/bin -lapt-pkg -lapt-inst -lssl -lcrypto -o s3 s3.o s3_main.o connect.o

%.o: %.cc
	 gcc -I /root/apt-debian-sid/build/include -I /usr/include -I./ -c $<