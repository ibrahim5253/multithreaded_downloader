#include <sys/types.h>
#include <poll.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>	
#include <regex>
#include <string>
#include <iostream>
#include <thread>
#include <sstream>
#include <chrono>

#define HEADER_SIZE 2048  // Typical header size : 2KB

using namespace std;

struct addrinfo *p;
string host, file_path, file_name;
long chunk_size;
vector<FILE*> f;
long time_taken = 0;
int N;

void setup(struct addrinfo *hint)
{
	memset(hint, 0, sizeof(*hint));
	hint->ai_family   = AF_INET;
	hint->ai_socktype = SOCK_STREAM; 
}

string range_request(long start, long end)
{
	return string("GET " + file_path + " HTTP/1.1\r\n" + "Host: "+host +
		         "\r\nRange: bytes="+to_string(start)+"-"+to_string(end)+"\r\n\r\n");
}

void print_progress(string s, double thrput, long dd)
{
	regex r("[\\s\\S]*Content-Range\\s*:\\s*bytes\\s*([0-9-/]+)[\\s\\S]*");
	cmatch c;
	if (regex_match(s.c_str(), c, r)) {
		printf("Bytes Received: %s.\tThroughput: %.2f kB/s. Bytes: %ld\n", string(c[1]).c_str(), thrput*1000.0/1024.0, dd);
	}
}

void download_chunk (int id, long start, long end)
{
	string fname = string("file")+to_string(id);
	f[id] = fopen(fname.c_str(),"ab+");

	auto begin_t = chrono::high_resolution_clock::now();

	int sd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
	connect(sd, p->ai_addr, p->ai_addrlen);

	// Send a Range request
	send(sd, range_request(start,end).c_str(), HEADER_SIZE, 0);

	char chunk;
	char last[5];
	long c=0;
	char header[HEADER_SIZE];

	// Remove the header
	while (recv(sd, &chunk, 1, MSG_WAITALL) > 0) {  
		if (c>=4) last[0]=last[1], last[1]=last[2], last[2]=last[3], last[3]=chunk;
		else last[c] = chunk;
		header[c]=chunk;
		if (c>4 && memcmp(last,"\r\n\r\n", 4)==0) break; // read till the end of the header
		++c;
	}
	header[c]='\0';

	// Write the data byte-by-byte to a file
	long d=0;
	while (recv(sd, &chunk, 1, MSG_WAITALL) > 0) {
		++d;
		fwrite(&chunk, 1,1, f[id]);
	}
	auto tt = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now()-begin_t).count();
	time_taken += tt;
	fclose(f[id]);
	double thrput = (c+d)/static_cast<double>(tt);
	print_progress(header, thrput, d);
}

void mergefiles()
{
	string cmd = "cat";
	for (int i=0; i<N; ++i) cmd.append(" file"+to_string(i));
	cmd.append(" > "+file_name);
	system(cmd.c_str());
}

void print_checksum()
{
	system(string("md5sum "+file_name).c_str());
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s URL N\n", argv[0]);
		return 0;
	}

	// Extract the host address, and file path
	string pat = "(?:https?://)([^/]+)(.*)";
	regex r(pat);
	cmatch m;
	if (!regex_match(argv[1], m, r)) {
		fprintf(stderr, "Invalid URL.\n");
		return 0;
	}

	host = m[1], file_path = m[2];

	// Get the file name from the file path
	int pos;
	cout << file_path << endl;
       	file_name = file_path.substr((pos=file_path.find_last_of("/"))==string::npos?0:pos+1);
	if (file_name=="") file_name = "downloaded_file";

	N = atoi(argv[2]);  // number of connections

	// Lookup the host ip and establish a connection
	struct addrinfo hint, *res;
	int status, sd;
	setup(&hint);

	if ((status=getaddrinfo(host.c_str(), "80", &hint, &res))!=0) { 
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status)); 
		return 0; 
	} 

	for (p=res; p!=NULL; p=p->ai_next) {
		sd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sd==-1) continue;
		if (connect(sd, p->ai_addr, p->ai_addrlen) == 0)
			break;
		close(sd);
	}

	if (p==NULL) {
		fprintf(stderr, "Could not resolve server ip.\n");
		return 0;
	}

	// HEAD Request
	string head_request = "HEAD " + file_path + " HTTP/1.1\r\n" + "Host: "+host+"\r\n\r\n";

	send(sd, head_request.c_str(), HEADER_SIZE, 0);

	char response[HEADER_SIZE];
	recv(sd, response, HEADER_SIZE, 0);

	// Extract the file size
	pat = "[\\s\\S]*Content-Length\\s*:\\s*([0-9]+)[\\s\\S]*";
	r = pat;
	if (!regex_match(response, m, r)) {
		fprintf(stderr, "Couldn't determine file size.\n");
		return 0;
	}

	long file_size = stol(m[1]);

	cout << "File size:"<< file_size << "\n";

	chunk_size = ceil(file_size/static_cast<double>(atoi(argv[2])));

	f.resize(N);

	// Create N Parallel connections, each downloading a chunk
	vector<thread> connections;
	for (int i=0; i<N; ++i) 
		connections.push_back( thread(download_chunk, i, i*chunk_size, min(file_size, (i+1)*chunk_size)-1) );

	for (auto &t:connections)
		t.join();

	// Merge the individual chunks
	mergefiles();
	
	freeaddrinfo(res);

	printf("Time taken: %.2f s\n", time_taken/1000.0);

	// Remove the temporaries and print the checksum
	system("rm file*");
	print_checksum();
	
	return 0;
}
