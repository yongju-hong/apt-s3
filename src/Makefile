
default: s3

clean:
	rm *.o s3

s3: s3_main.o s3.o connect.o
	 g++ -lapt-pkg -lapt-inst -lssl -lcrypto -o s3 s3.o s3_main.o connect.o

%.o: %.cc
	 gcc  -I /usr/include -I./ -g -c $<

install: s3
	cp s3 /usr/lib/apt/methods/