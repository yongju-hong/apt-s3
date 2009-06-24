
default: s3

clean:
	rm *.o s3

s3: s3_main.o s3.o connect.o
	 g++ -L/usr/lib -L/sw/lib -lapt-pkg -lapt-inst -lssl -lcrypto -o s3 s3.o s3_main.o connect.o

%.o: %.cc
	 gcc -I /sw/include/ -I /usr/include -I./ -c $<