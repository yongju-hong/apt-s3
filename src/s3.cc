/*  Copyright (C) 2008,2012 Kyle Shank
    This file is part of apt-s3.

    apt-s3 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    apt-s3 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with apt-s3.  If not, see <http://www.gnu.org/licenses/>.
*/
// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: http.cc,v 1.59 2004/05/08 19:42:35 mdz Exp $
/* ######################################################################

   HTTP Acquire Method - This is the HTTP aquire method for APT.
   
   It uses HTTP/1.1 and many of the fancy options there-in, such as
   pipelining, range, if-range and so on. 

   It is based on a doubly buffered select loop. A groupe of requests are 
   fed into a single output buffer that is constantly fed out the 
   socket. This provides ideal pipelining as in many cases all of the
   requests will fit into a single packet. The input socket is buffered 
   the same way and fed into the fd for the file (may be a pipe in future).
   
   This double buffering provides fairly substantial transfer rates,
   compared to wget the http method is about 4% faster. Most importantly,
   when HTTP is compared with FTP as a protocol the speed difference is
   huge. In tests over the internet from two sites to llug (via ATM) this
   program got 230k/s sustained http transfer rates. FTP on the other 
   hand topped out at 170k/s. That combined with the time to setup the
   FTP connection makes HTTP a vastly superior protocol.
      
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <apt-pkg/fileutl.h>
#include <apt-pkg/acquire-method.h>
#include <apt-pkg/error.h>
#include <apt-pkg/hashes.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <apti18n.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <curl/curl.h>

#include <iostream>
#include <fstream>

// Internet stuff
#include <netdb.h>

#include "config.h"
#include "connect.h"
#include "rfc2553emu.h"
#include "s3.h"

#define SLEN 1024

void doEncrypt(char * kString,char * str, const char * secretKey);
									/*}}}*/
using namespace std;

string HttpMethod::FailFile;
int HttpMethod::FailFd = -1;
time_t HttpMethod::FailTime = 0;
unsigned long PipelineDepth = 10;
unsigned long TimeOut = 120;
bool Debug = false;
URI Proxy;

unsigned long CircleBuf::BwReadLimit=0;
unsigned long CircleBuf::BwTickReadData=0;
struct timeval CircleBuf::BwReadTick={0,0};
const unsigned int CircleBuf::BW_HZ=10;
  
// CircleBuf::CircleBuf - Circular input buffer				/*{{{*/
// ---------------------------------------------------------------------
/* */
CircleBuf::CircleBuf(unsigned long Size) : Size(Size), Hash(0)
{
   Buf = new unsigned char[Size];
   Reset();

   CircleBuf::BwReadLimit = _config->FindI("Acquire::http::Dl-Limit",0)*1024;
}
									/*}}}*/
// CircleBuf::Reset - Reset to the default state			/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::Reset()
{
   InP = 0;
   OutP = 0;
   StrPos = 0;
   MaxGet = (unsigned int)-1;
   OutQueue = string();
   if (Hash != 0)
   {
      delete Hash;
      Hash = new Hashes;
   }   
};
									/*}}}*/
// CircleBuf::Read - Read from a FD into the circular buffer		/*{{{*/
// ---------------------------------------------------------------------
/* This fills up the buffer with as much data as is in the FD, assuming it
   is non-blocking.. */
bool CircleBuf::Read(int Fd)
{
   unsigned long BwReadMax;

   while (1)
   {
      // Woops, buffer is full
      if (InP - OutP == Size)
	 return true;

      // what's left to read in this tick
      BwReadMax = CircleBuf::BwReadLimit/BW_HZ;

      if(CircleBuf::BwReadLimit) {
	 struct timeval now;
	 gettimeofday(&now,0);

	 unsigned long d = (now.tv_sec-CircleBuf::BwReadTick.tv_sec)*1000000 +
	    now.tv_usec-CircleBuf::BwReadTick.tv_usec;
	 if(d > 1000000/BW_HZ) {
	    CircleBuf::BwReadTick = now;
	    CircleBuf::BwTickReadData = 0;
	 } 
	 
	 if(CircleBuf::BwTickReadData >= BwReadMax) {
	    usleep(1000000/BW_HZ);
	    return true;
	 }
      }

      // Write the buffer segment
      int Res;
      if(CircleBuf::BwReadLimit) {
	 Res = read(Fd,Buf + (InP%Size), 
		    BwReadMax > LeftRead() ? LeftRead() : BwReadMax);
      } else
	 Res = read(Fd,Buf + (InP%Size),LeftRead());
      
      if(Res > 0 && BwReadLimit > 0) 
	 CircleBuf::BwTickReadData += Res;
    
      if (Res == 0)
	 return false;
      if (Res < 0)
      {
	 if (errno == EAGAIN)
	    return true;
	 return false;
      }

      if (InP == 0)
	 gettimeofday(&Start,0);
      InP += Res;
   }
}
									/*}}}*/
// CircleBuf::Read - Put the string into the buffer			/*{{{*/
// ---------------------------------------------------------------------
/* This will hold the string in and fill the buffer with it as it empties */
bool CircleBuf::Read(string Data)
{
   OutQueue += Data;
   FillOut();
   return true;
}
									/*}}}*/
// CircleBuf::FillOut - Fill the buffer from the output queue		/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::FillOut()
{
   if (OutQueue.empty() == true)
      return;
   while (1)
   {
      // Woops, buffer is full
      if (InP - OutP == Size)
	 return;
      
      // Write the buffer segment
      unsigned long Sz = LeftRead();
      if (OutQueue.length() - StrPos < Sz)
	 Sz = OutQueue.length() - StrPos;
      memcpy(Buf + (InP%Size),OutQueue.c_str() + StrPos,Sz);
      
      // Advance
      StrPos += Sz;
      InP += Sz;
      if (OutQueue.length() == StrPos)
      {
	 StrPos = 0;
	 OutQueue = "";
	 return;
      }
   }
}
									/*}}}*/
// CircleBuf::Write - Write from the buffer into a FD			/*{{{*/
// ---------------------------------------------------------------------
/* This empties the buffer into the FD. */
bool CircleBuf::Write(int Fd)
{
   while (1)
   {
      FillOut();
      
      // Woops, buffer is empty
      if (OutP == InP)
	 return true;
      
      if (OutP == MaxGet)
	 return true;
      
      // Write the buffer segment
      int Res;
      Res = write(Fd,Buf + (OutP%Size),LeftWrite());

      if (Res == 0)
	 return false;
      if (Res < 0)
      {
	 if (errno == EAGAIN)
	    return true;
	 
	 return false;
      }
      
      if (Hash != 0)
	 Hash->Add(Buf + (OutP%Size),Res);
      
      OutP += Res;
   }
}
									/*}}}*/
// CircleBuf::WriteTillEl - Write from the buffer to a string		/*{{{*/
// ---------------------------------------------------------------------
/* This copies till the first empty line */
bool CircleBuf::WriteTillEl(string &Data,bool Single)
{
   // We cheat and assume it is unneeded to have more than one buffer load
   for (unsigned long I = OutP; I < InP; I++)
   {      
      if (Buf[I%Size] != '\n')
	 continue;
      ++I;
      
      if (Single == false)
      {
         if (I < InP  && Buf[I%Size] == '\r')
            ++I;
         if (I >= InP || Buf[I%Size] != '\n')
            continue;
         ++I;
      }
      
      Data = "";
      while (OutP < I)
      {
	 unsigned long Sz = LeftWrite();
	 if (Sz == 0)
	    return false;
	 if (I - OutP < Sz)
	    Sz = I - OutP;
	 Data += string((char *)(Buf + (OutP%Size)),Sz);
	 OutP += Sz;
      }
      return true;
   }      
   return false;
}
									/*}}}*/
// CircleBuf::Stats - Print out stats information			/*{{{*/
// ---------------------------------------------------------------------
/* */
void CircleBuf::Stats()
{
   if (InP == 0)
      return;
   
   struct timeval Stop;
   gettimeofday(&Stop,0);
/*   float Diff = Stop.tv_sec - Start.tv_sec + 
             (float)(Stop.tv_usec - Start.tv_usec)/1000000;
   clog << "Got " << InP << " in " << Diff << " at " << InP/Diff << endl;*/
}
									/*}}}*/

// ServerState::ServerState - Constructor				/*{{{*/
// ---------------------------------------------------------------------
/* */
ServerState::ServerState(URI Srv,HttpMethod *Owner) : Owner(Owner),
                        In(64*1024), Out(4*1024),
                        ServerName(Srv)
{
   Reset();
}
									/*}}}*/
// ServerState::Open - Open a connection to the server			/*{{{*/
// ---------------------------------------------------------------------
/* This opens a connection to the server. */
bool ServerState::Open()
{
   // Use the already open connection if possible.
   if (ServerFd != -1)
      return true;
   
   Close();
   In.Reset();
   Out.Reset();
   Persistent = true;
   
   // Determine the proxy setting
   if (getenv("http_proxy") == 0)
   {
      string DefProxy = _config->Find("Acquire::http::Proxy");
      string SpecificProxy = _config->Find("Acquire::http::Proxy::" + ServerName.Host);
      if (SpecificProxy.empty() == false)
      {
	 if (SpecificProxy == "DIRECT")
	    Proxy = "";
	 else
	    Proxy = SpecificProxy;
      }   
      else
	 Proxy = DefProxy;
   }
   else
      Proxy = getenv("http_proxy");
   
   // Parse no_proxy, a , separated list of domains
   if (getenv("no_proxy") != 0)
   {
      if (CheckDomainList(ServerName.Host,getenv("no_proxy")) == true)
	 Proxy = "";
   }
   
   // Determine what host and port to use based on the proxy settings
   int Port = 0;
   string Host;   
   if (Proxy.empty() == true || Proxy.Host.empty() == true)
   {
      if (ServerName.Port != 0)
	 Port = ServerName.Port;
      Host = ServerName.Host;
   }
   else
   {
      if (Proxy.Port != 0)
	 Port = Proxy.Port;
      Host = Proxy.Host;
   }
   
   // Connect to the remote server
   if (Connect(Host,Port,"http",80,ServerFd,TimeOut,Owner) == false)
      return false;
   
   return true;
}
									/*}}}*/
// ServerState::Close - Close a connection to the server		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::Close()
{
   close(ServerFd);
   ServerFd = -1;
   return true;
}
									/*}}}*/
// ServerState::RunHeaders - Get the headers before the data		/*{{{*/
// ---------------------------------------------------------------------
/* Returns 0 if things are OK, 1 if an IO error occurred and 2 if a header
   parse error occurred */
int ServerState::RunHeaders()
{
   State = Header;
   
   Owner->Status(_("Waiting for headers"));

   Major = 0; 
   Minor = 0; 
   Result = 0; 
   Size = 0; 
   StartPos = 0;
   Encoding = Closes;
   HaveContent = false;
   time(&Date);

   do
   {
      string Data;
      if (In.WriteTillEl(Data) == false)
	 continue;

      if (Debug == true)
	 cerr << Data;
      
      for (string::const_iterator I = Data.begin(); I < Data.end(); I++)
      {
	 string::const_iterator J = I;
	 for (; J != Data.end() && *J != '\n' && *J != '\r';J++);
	 if (HeaderLine(string(I,J)) == false)
	    return 2;
	 I = J;
      }

      // 100 Continue is a Nop...
      if (Result == 100)
	 continue;
      
      // Tidy up the connection persistance state.
      if (Encoding == Closes && HaveContent == true)
	 Persistent = false;
      
      return 0;
   }
   while (Owner->Go(false,this) == true);
   
   return 1;
}
									/*}}}*/
// ServerState::RunData - Transfer the data from the socket		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::RunData()
{
   State = Data;
   
   if (Debug == true) cerr << "Response" << endl;

   // Chunked transfer encoding is fun..
   if (Encoding == Chunked)
   {
      while (1)
      {
	 // Grab the block size
	 bool Last = true;
	 string Data;
	 In.Limit(-1);
	 do
	 {
	    if (In.WriteTillEl(Data,true) == true)
	       break;
            if (Debug == true) cerr << Data;
	 }
	 while ((Last = Owner->Go(false,this)) == true);

         if (Debug == true) cerr << Data;

	 if (Last == false)
	    return false;
	 	 
	 // See if we are done
	 unsigned long Len = strtol(Data.c_str(),0,16);
	 if (Len == 0)
	 {
	    In.Limit(-1);
	    
	    // We have to remove the entity trailer
	    Last = true;
	    do
	    {
	       if (In.WriteTillEl(Data,true) == true && Data.length() <= 2)
		  break;
	    }
	    while ((Last = Owner->Go(false,this)) == true);
	    if (Last == false)
	       return false;
	    return !_error->PendingError();
	 }
	 
	 // Transfer the block
	 In.Limit(Len);
	 while (Owner->Go(true,this) == true)
	    if (In.IsLimit() == true)
	       break;
	 
	 // Error
	 if (In.IsLimit() == false)
	    return false;
	 
	 // The server sends an extra new line before the next block specifier..
	 In.Limit(-1);
	 Last = true;
	 do
	 {
	    if (In.WriteTillEl(Data,true) == true)
	       break;
	 }
	 while ((Last = Owner->Go(false,this)) == true);
	 if (Last == false)
	    return false;
      }
   }
   else
   {
      /* Closes encoding is used when the server did not specify a size, the
         loss of the connection means we are done */
      if (Encoding == Closes)
	 In.Limit(-1);
      else
	 In.Limit(Size - StartPos);
      
      // Just transfer the whole block.
      do
      {
	 if (In.IsLimit() == false)
	    continue;
	 
	 In.Limit(-1);
	 return !_error->PendingError();
      }
      while (Owner->Go(true,this) == true);
   }

   return Owner->Flush(this) && !_error->PendingError();
}
									/*}}}*/
// ServerState::HeaderLine - Process a header line			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool ServerState::HeaderLine(string Line)
{
   if (Line.empty() == true)
      return true;

   // The http server might be trying to do something evil.
   if (Line.length() >= MAXLEN)
      return _error->Error(_("Got a single header line over %u chars"),MAXLEN);

   string::size_type Pos = Line.find(' ');
   if (Pos == string::npos || Pos+1 > Line.length())
   {
      // Blah, some servers use "connection:closes", evil.
      Pos = Line.find(':');
      if (Pos == string::npos || Pos + 2 > Line.length())
	 return _error->Error(_("Bad header line"));
      Pos++;
   }

   // Parse off any trailing spaces between the : and the next word.
   string::size_type Pos2 = Pos;
   while (Pos2 < Line.length() && isspace(Line[Pos2]) != 0)
      Pos2++;
      
   string Tag = string(Line,0,Pos);
   string Val = string(Line,Pos2);
   
   if (stringcasecmp(Tag.c_str(),Tag.c_str()+4,"HTTP") == 0)
   {
      // Evil servers return no version
      if (Line[4] == '/')
      {
	 if (sscanf(Line.c_str(),"HTTP/%u.%u %u %[^\n]",&Major,&Minor,
		    &Result,Code) != 4)
	    return _error->Error(_("The HTTP server sent an invalid reply header"));
      }
      else
      {
	 Major = 0;
	 Minor = 9;
	 if (sscanf(Line.c_str(),"HTTP %u %[^\n]",&Result,Code) != 2)
	    return _error->Error(_("The HTTP server sent an invalid reply header"));
      }

      /* Check the HTTP response header to get the default persistance
         state. */
      if (Major < 1)
	 Persistent = false;
      else
      {
	 if (Major == 1 && Minor <= 0)
	    Persistent = false;
	 else
	    Persistent = true;
      }

      return true;
   }      
      
   if (stringcasecmp(Tag,"Content-Length:") == 0)
   {
      if (Encoding == Closes)
	 Encoding = Stream;
      HaveContent = true;
      
      // The length is already set from the Content-Range header
      if (StartPos != 0)
	 return true;
      
      if (sscanf(Val.c_str(),"%lu",&Size) != 1)
	 return _error->Error(_("The HTTP server sent an invalid Content-Length header"));
      return true;
   }

   if (stringcasecmp(Tag,"Content-Type:") == 0)
   {
      HaveContent = true;
      return true;
   }
   
   if (stringcasecmp(Tag,"Content-Range:") == 0)
   {
      HaveContent = true;
      
      if (sscanf(Val.c_str(),"bytes %lu-%*u/%lu",&StartPos,&Size) != 2)
	 return _error->Error(_("The HTTP server sent an invalid Content-Range header"));
      if ((unsigned)StartPos > Size)
	 return _error->Error(_("This HTTP server has broken range support"));
      return true;
   }
   
   if (stringcasecmp(Tag,"Transfer-Encoding:") == 0)
   {
      HaveContent = true;
      if (stringcasecmp(Val,"chunked") == 0)
	 Encoding = Chunked;      
      return true;
   }

   if (stringcasecmp(Tag,"Connection:") == 0)
   {
      if (stringcasecmp(Val,"close") == 0)
	 Persistent = false;
      if (stringcasecmp(Val,"keep-alive") == 0)
	 Persistent = true;
      return true;
   }
   
   if (stringcasecmp(Tag,"Last-Modified:") == 0)
   {
      if (StrToTime(Val,Date) == false)
	 return _error->Error(_("Unknown date format"));
      return true;
   }

   return true;
}
									/*}}}*/

// HttpMethod::SendReq - Send the HTTP request				/*{{{*/
// ---------------------------------------------------------------------
/* This places the http request in the outbound buffer */
void HttpMethod::SendReq(FetchItem *Itm,CircleBuf &Out)
{
   URI Uri = Itm->Uri;

   // The HTTP server expects a hostname with a trailing :port
   char Buf[1000];
   string ProperHost = Uri.Host;
   if (Uri.Port != 0)
   {
      sprintf(Buf,":%u",Uri.Port);
      ProperHost += Buf;
   }   
      
   // Just in case.
   if (Itm->Uri.length() >= sizeof(Buf))
       abort();
       
   // Stupid workaround for https://forums.aws.amazon.com/thread.jspa?threadID=55746
   // tl;dr s3 does not play nice with filenames that have a + sign in them
   string normalized_path = QuoteString(Uri.Path, "~");
   size_t f_plus = normalized_path.find("+");
   size_t f_rep;
   while (f_plus != string::npos){
      f_rep = f_plus;
      normalized_path.replace(f_rep, 1, "%2b");
      f_plus = normalized_path.find("+", f_plus + 1);
   }
    //if (Uri.Path != normalized_path){
    //    cerr << "\nTransforming " << Uri.Path << " to " << normalized_path << "\n";
    //}

   /* Build the request. We include a keep-alive header only for non-proxy
      requests. This is to tweak old http/1.0 servers that do support keep-alive
      but not HTTP/1.1 automatic keep-alive. Doing this with a proxy server 
      will glitch HTTP/1.0 proxies because they do not filter it out and 
      pass it on, HTTP/1.1 says the connection should default to keep alive
      and we expect the proxy to do this */
   if (Proxy.empty() == true || Proxy.Host.empty())
      sprintf(Buf,"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n",
	      normalized_path.c_str(),ProperHost.c_str());
   else
   {
      /* Generate a cache control header if necessary. We place a max
       	 cache age on index files, optionally set a no-cache directive
       	 and a no-store directive for archives. */
      sprintf(Buf,"GET %s HTTP/1.1\r\nHost: %s\r\n",
	      Itm->Uri.c_str(),ProperHost.c_str());
      // only generate a cache control header if we actually want to 
      // use a cache
      if (_config->FindB("Acquire::http::No-Cache",false) == false)
      {
	 if (Itm->IndexFile == true)
	    sprintf(Buf+strlen(Buf),"Cache-Control: max-age=%u\r\n",
		    _config->FindI("Acquire::http::Max-Age",0));
	 else
	 {
	    if (_config->FindB("Acquire::http::No-Store",false) == true)
	       strcat(Buf,"Cache-Control: no-store\r\n");
	 }	 
      }
   }
   // generate a no-cache header if needed
   if (_config->FindB("Acquire::http::No-Cache",false) == true)
      strcat(Buf,"Cache-Control: no-cache\r\nPragma: no-cache\r\n");

   
   string Req = Buf;

   // Check for a partial file
   struct stat SBuf;
   if (stat(Itm->DestFile.c_str(),&SBuf) >= 0 && SBuf.st_size > 0)
   {
      // In this case we send an if-range query with a range header
      sprintf(Buf,"Range: bytes=%li-\r\nIf-Range: %s\r\n",(long)SBuf.st_size - 1,
	      TimeRFC1123(SBuf.st_mtime).c_str());
      Req += Buf;
   }
   else
   {
      if (Itm->LastModified != 0)
      {
	 sprintf(Buf,"If-Modified-Since: %s\r\n",TimeRFC1123(Itm->LastModified).c_str());
	 Req += Buf;
      }
   }

   if (Proxy.User.empty() == false || Proxy.Password.empty() == false)
      Req += string("Proxy-Authorization: Basic ") + 
          Base64Encode(Proxy.User + ":" + Proxy.Password) + "\r\n";
	
   /* S3 Specific */
   time_t rawtime = 0;
   struct tm * timeinfo = NULL;
   char buffer [80] = { 0 };
   char* wday = NULL;
   char* mon = NULL;

   time( &rawtime);
   timeinfo = gmtime( &rawtime);

   // strftime does not seem to honour set_locale(LC_ALL, "") or
   // set_locale(LC_TIME, ""). So convert day of week by hand.
   switch (timeinfo->tm_wday) {
   case 0: wday = (char*)"Sun"; break;
   case 1: wday = (char*)"Mon"; break;
   case 2: wday = (char*)"Tue"; break;
   case 3: wday = (char*)"Wed"; break;
   case 4: wday = (char*)"Thu"; break;
   case 5: wday = (char*)"Fri"; break;
   case 6: wday = (char*)"Sat"; break;
   }

   switch (timeinfo->tm_mon) {
   case 0: mon = (char*)"Jan"; break;
   case 1: mon = (char*)"Fab"; break;
   case 2: mon = (char*)"Mar"; break;
   case 3: mon = (char*)"Apr"; break;
   case 4: mon = (char*)"May"; break;
   case 5: mon = (char*)"Jun"; break;
   case 6: mon = (char*)"Jul"; break;
   case 7: mon = (char*)"Aug"; break;
   case 8: mon = (char*)"Sep"; break;
   case 9: mon = (char*)"Oct"; break;
   case 10: mon = (char*)"Nov"; break;
   case 11: mon = (char*)"Dec"; break;
   }

   strcat(buffer, wday);
   strftime(buffer+3, 77, ", %d ", timeinfo);
   strcat(buffer+8, mon);
   strftime(buffer+11, 69, " %Y %T %z", timeinfo);
   string dateString((const char*)buffer);
   Req += "Date: " + dateString + "\r\n";

   string extractedPassword;
   if (Uri.Password.empty() && NULL == getenv("AWS_SECRET_ACCESS_KEY")) {
     cerr << "E: No AWS_SECRET_ACCESS_KEY set" << endl;
     exit(1);
   } else if(Uri.Password.empty()) {
     extractedPassword = getenv("AWS_SECRET_ACCESS_KEY");
   } else {
     if(Uri.Password.at(0) == '['){
       extractedPassword = Uri.Password.substr(1,Uri.Password.size()-2);
     }else{
       extractedPassword = Uri.Password;
     }
   }

   char headertext[SLEN], signature[SLEN];
   sprintf(headertext,"GET\n\n\n%s\n%s", dateString.c_str(), normalized_path.c_str());
   doEncrypt(headertext, signature, extractedPassword.c_str());

   string signatureString(signature);
  string user;
  if (Uri.User.empty() && NULL == getenv("AWS_ACCESS_KEY_ID")) {
    cerr << "E: No AWS_ACCESS_KEY_ID set" << endl;
    exit(1);
  } else if (Uri.User.empty()) {
    user = getenv("AWS_ACCESS_KEY_ID");
  } else {
    user = Uri.User;
  }

  //cerr << "user " << user << "\n";
	Req += "Authorization: AWS " + user + ":" + signatureString + "\r\n";
   	Req += "User-Agent: Ubuntu APT-HTTP/1.3 ("VERSION")\r\n\r\n";

   if (Debug == true)
     cerr << "Request" << endl << Req << endl;

   Out.Read(Req);
}

void doEncrypt(char *kString, char *sigString, const char* secretKey){
	HMAC_CTX hctx;
	BIO *bio, *b64;
	char *sigptr;
	long siglen = -1;
	unsigned int rizlen;
	char skey[SLEN];
	unsigned char results[SLEN];

	// Initialize SHA1 encryption
	sprintf(skey, "%s", secretKey);
	HMAC_CTX_init(&hctx);
	HMAC_Init(&hctx, skey, (int)strlen((char *)skey), EVP_sha1());

	// Encrypt
	HMAC(EVP_sha1(), skey, (int)strlen((char *)skey), (unsigned char *)kString,
			(int)strlen((char *)kString), results, &rizlen);

	// Base 64 Encode
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new(BIO_s_mem());
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	bio = BIO_push(b64, bio);
	BIO_write(bio, results, rizlen);
	BIO_flush(bio);

	siglen = BIO_get_mem_data(bio, &sigptr);
	memcpy(sigString, sigptr, siglen);
	sigString[siglen] = '\0';

	// Clean up Encryption, Encoding
	BIO_free_all(bio);
	HMAC_CTX_cleanup(&hctx);
}
									/*}}}*/
// HttpMethod::Go - Run a single loop					/*{{{*/
// ---------------------------------------------------------------------
/* This runs the select loop over the server FDs, Output file FDs and
   stdin. */
bool HttpMethod::Go(bool ToFile,ServerState *Srv)
{
   // Server has closed the connection
   if (Srv->ServerFd == -1 && (Srv->In.WriteSpace() == false || 
			       ToFile == false))
      return false;
   
   fd_set rfds,wfds;
   FD_ZERO(&rfds);
   FD_ZERO(&wfds);
   
   /* Add the server. We only send more requests if the connection will 
      be persisting */
   if (Srv->Out.WriteSpace() == true && Srv->ServerFd != -1 
       && Srv->Persistent == true)
      FD_SET(Srv->ServerFd,&wfds);
   if (Srv->In.ReadSpace() == true && Srv->ServerFd != -1)
      FD_SET(Srv->ServerFd,&rfds);
   
   // Add the file
   int FileFD = -1;
   if (File != 0)
      FileFD = File->Fd();
   
   if (Srv->In.WriteSpace() == true && ToFile == true && FileFD != -1)
      FD_SET(FileFD,&wfds);
   
   // Add stdin
   FD_SET(STDIN_FILENO,&rfds);
	  
   // Figure out the max fd
   int MaxFd = FileFD;
   if (MaxFd < Srv->ServerFd)
      MaxFd = Srv->ServerFd;

   // Select
   struct timeval tv;
   tv.tv_sec = TimeOut;
   tv.tv_usec = 0;
   int Res = 0;
   if ((Res = select(MaxFd+1,&rfds,&wfds,0,&tv)) < 0)
   {
      if (errno == EINTR)
	 return true;
      return _error->Errno("select",_("Select failed"));
   }
   
   if (Res == 0)
   {
      _error->Error(_("Connection timed out"));
      return ServerDie(Srv);
   }
   
   // Handle server IO
   if (Srv->ServerFd != -1 && FD_ISSET(Srv->ServerFd,&rfds))
   {
      errno = 0;
      if (Srv->In.Read(Srv->ServerFd) == false)
	 return ServerDie(Srv);
   }
	 
   if (Srv->ServerFd != -1 && FD_ISSET(Srv->ServerFd,&wfds))
   {
      errno = 0;
      if (Srv->Out.Write(Srv->ServerFd) == false)
	 return ServerDie(Srv);
   }

   // Send data to the file
   if (FileFD != -1 && FD_ISSET(FileFD,&wfds))
   {
      if (Srv->In.Write(FileFD) == false)
	 return _error->Errno("write",_("Error writing to output file"));
   }

   // Handle commands from APT
   if (FD_ISSET(STDIN_FILENO,&rfds))
   {
      if (Run(true) != -1)
	 exit(100);
   }   
       
   return true;
}
									/*}}}*/
// HttpMethod::Flush - Dump the buffer into the file			/*{{{*/
// ---------------------------------------------------------------------
/* This takes the current input buffer from the Server FD and writes it
   into the file */
bool HttpMethod::Flush(ServerState *Srv)
{
   if (File != 0)
   {
      // on GNU/kFreeBSD, apt dies on /dev/null because non-blocking
      // can't be set
      if (File->Name() != "/dev/null")
	 SetNonBlock(File->Fd(),false);
      if (Srv->In.WriteSpace() == false)
	 return true;
      
      while (Srv->In.WriteSpace() == true)
      {
	 if (Srv->In.Write(File->Fd()) == false)
	    return _error->Errno("write",_("Error writing to file"));
	 if (Srv->In.IsLimit() == true)
	    return true;
      }

      if (Srv->In.IsLimit() == true || Srv->Encoding == ServerState::Closes)
	 return true;
   }
   return false;
}
									/*}}}*/
// HttpMethod::ServerDie - The server has closed the connection.	/*{{{*/
// ---------------------------------------------------------------------
/* */
bool HttpMethod::ServerDie(ServerState *Srv)
{
   unsigned int LErrno = errno;
   
   // Dump the buffer to the file
   if (Srv->State == ServerState::Data)
   {
      // on GNU/kFreeBSD, apt dies on /dev/null because non-blocking
      // can't be set
      if (File->Name() != "/dev/null")
	 SetNonBlock(File->Fd(),false);
      while (Srv->In.WriteSpace() == true)
      {
	 if (Srv->In.Write(File->Fd()) == false)
	    return _error->Errno("write",_("Error writing to the file"));

	 // Done
	 if (Srv->In.IsLimit() == true)
	    return true;
      }
   }
   
   // See if this is because the server finished the data stream
   if (Srv->In.IsLimit() == false && Srv->State != ServerState::Header && 
       Srv->Encoding != ServerState::Closes)
   {
      Srv->Close();
      if (LErrno == 0)
	 return _error->Error(_("Error reading from server. Remote end closed connection"));
      errno = LErrno;
      return _error->Errno("read",_("Error reading from server"));
   }
   else
   {
      Srv->In.Limit(-1);

      // Nothing left in the buffer
      if (Srv->In.WriteSpace() == false)
	 return false;
      
      // We may have got multiple responses back in one packet..
      Srv->Close();
      return true;
   }
   
   return false;
}
									/*}}}*/
// HttpMethod::DealWithHeaders - Handle the retrieved header data	/*{{{*/
// ---------------------------------------------------------------------
/* We look at the header data we got back from the server and decide what
   to do. Returns 
     0 - File is open,
     1 - IMS hit
     3 - Unrecoverable error 
     4 - Error with error content page
     5 - Unrecoverable non-server error (close the connection) */
int HttpMethod::DealWithHeaders(FetchResult &Res,ServerState *Srv)
{
   // Not Modified
   if (Srv->Result == 304)
   {
      unlink(Queue->DestFile.c_str());
      Res.IMSHit = true;
      Res.LastModified = Queue->LastModified;
      return 1;
   }
   
   /* We have a reply we dont handle. This should indicate a perm server
      failure */
   if (Srv->Result < 200 || Srv->Result >= 300)
   {
      char err[255];
      snprintf(err,sizeof(err)-1,"HttpError%i",Srv->Result);
      // SetFailReason(err);
      _error->Error("%u %s",Srv->Result,Srv->Code);
      if (Srv->HaveContent == true)
	 return 4;
      return 3;
   }

   // This is some sort of 2xx 'data follows' reply
   Res.LastModified = Srv->Date;
   Res.Size = Srv->Size;
   
   // Open the file
   delete File;
   File = new FileFd(Queue->DestFile,FileFd::WriteAny);
   if (_error->PendingError() == true)
      return 5;

   FailFile = Queue->DestFile;
   FailFile.c_str();   // Make sure we dont do a malloc in the signal handler
   FailFd = File->Fd();
   FailTime = Srv->Date;
      
   // Set the expected size
   if (Srv->StartPos >= 0)
   {
      Res.ResumePoint = Srv->StartPos;
      ftruncate(File->Fd(),Srv->StartPos);
   }
      
   // Set the start point
   lseek(File->Fd(),0,SEEK_END);

   delete Srv->In.Hash;
   Srv->In.Hash = new Hashes;
   
   // Fill the Hash if the file is non-empty (resume)
   if (Srv->StartPos > 0)
   {
      lseek(File->Fd(),0,SEEK_SET);
      if (Srv->In.Hash->AddFD(File->Fd(),Srv->StartPos) == false)
      {
	 _error->Errno("read",_("Problem hashing file"));
	 return 5;
      }
      lseek(File->Fd(),0,SEEK_END);
   }
   
   SetNonBlock(File->Fd(),true);
   return 0;
}
									/*}}}*/
// HttpMethod::SigTerm - Handle a fatal signal				/*{{{*/
// ---------------------------------------------------------------------
/* This closes and timestamps the open file. This is neccessary to get 
   resume behavoir on user abort */
void HttpMethod::SigTerm(int)
{
   if (FailFd == -1)
      _exit(100);
   close(FailFd);
   
   // Timestamp
   struct utimbuf UBuf;
   UBuf.actime = FailTime;
   UBuf.modtime = FailTime;
   utime(FailFile.c_str(),&UBuf);
   
   _exit(100);
}
									/*}}}*/
// HttpMethod::Fetch - Fetch an item					/*{{{*/
// ---------------------------------------------------------------------
/* This adds an item to the pipeline. We keep the pipeline at a fixed
   depth. */
bool HttpMethod::Fetch(FetchItem *)
{
   if (Server == 0) 
      return true;

   // Queue the requests
   int Depth = -1;
   for (FetchItem *I = Queue; I != 0 && Depth < (signed)PipelineDepth; 
	I = I->Next, Depth++)
   {
      // If pipelining is disabled, we only queue 1 request
      if (Server->Pipeline == false && Depth >= 0)
	 break;
      
      // Make sure we stick with the same server
      if (Server->Comp(I->Uri) == false)
	 break;
      if (QueueBack == I)
      {
	 QueueBack = I->Next;
	 SendReq(I,Server->Out);
	 continue;
      }
   }
   
   return true;
};
									/*}}}*/
// HttpMethod::Configuration - Handle a configuration message		/*{{{*/
// ---------------------------------------------------------------------
/* We stash the desired pipeline depth */
bool HttpMethod::Configuration(string Message)
{
   if (pkgAcqMethod::Configuration(Message) == false)
      return false;
   
   TimeOut = _config->FindI("Acquire::http::Timeout",TimeOut);
   PipelineDepth = _config->FindI("Acquire::http::Pipeline-Depth",
				  PipelineDepth);
   Debug = _config->FindB("Debug::Acquire::http",false);
   return true;
}
									/*}}}*/
// HttpMethod::Loop - Main loop						/*{{{*/
// ---------------------------------------------------------------------
/* */
int HttpMethod::Loop()
{
   signal(SIGTERM,SigTerm);
   signal(SIGINT,SigTerm);
   
   Server = 0;
   
   int FailCounter = 0;
   while (1)
   {      
      // We have no commands, wait for some to arrive
      if (Queue == 0)
      {
	 if (WaitFd(STDIN_FILENO) == false)
	    return 0;
      }
      
      /* Run messages, we can accept 0 (no message) if we didn't
         do a WaitFd above.. Otherwise the FD is closed. */
      
      int Result = Run(true);
      if (Result != -1 && (Result != 0 || Queue == 0))
	 return 100;

      if (Queue == 0)
	 continue;
      
      // Connect to the server
      if (Server == 0 || Server->Comp(Queue->Uri) == false)
      {
	 delete Server;
	 Server = new ServerState(Queue->Uri,this);
      }
      /* If the server has explicitly said this is the last connection
         then we pre-emptively shut down the pipeline and tear down 
	 the connection. This will speed up HTTP/1.0 servers a tad
	 since we don't have to wait for the close sequence to
         complete */
      if (Server->Persistent == false)
	 Server->Close();
      
      // Reset the pipeline
      if (Server->ServerFd == -1)
	 QueueBack = Queue;	 
	 
      // Connnect to the host
      if (Server->Open() == false)
      {
	 Fail(true);
	 delete Server;
	 Server = 0;
	 continue;
      }

      // Fill the pipeline.
      Fetch(0);
      
      // Fetch the next URL header data from the server.
      switch (Server->RunHeaders())
      {
	 case 0:
	 break;
	 
	 // The header data is bad
	 case 2:
	 {
	    _error->Error(_("Bad header data"));
	    Fail(true);
	    RotateDNS();
	    continue;
	 }
	 
	 // The server closed a connection during the header get..
	 default:
	 case 1:
	 {
	    FailCounter++;
	    _error->Discard();
	    Server->Close();
	    Server->Pipeline = false;
	    
	    if (FailCounter >= 2)
	    {
	       Fail(_("Connection failed"),true);
	       FailCounter = 0;
	    }
	    
	    RotateDNS();
	    continue;
	 }
      };

      // Decide what to do.
      FetchResult Res;
      Res.Filename = Queue->DestFile;
      switch (DealWithHeaders(Res,Server))
      {
	 // Ok, the file is Open
	 case 0:
	 {
	    URIStart(Res);

	    // Run the data
	    bool Result =  Server->RunData();

	    /* If the server is sending back sizeless responses then fill in
	       the size now */
	    if (Res.Size == 0)
	       Res.Size = File->Size();
	    
	    // Close the file, destroy the FD object and timestamp it
	    FailFd = -1;
	    delete File;
	    File = 0;
	    
	    // Timestamp
	    struct utimbuf UBuf;
	    time(&UBuf.actime);
	    UBuf.actime = Server->Date;
	    UBuf.modtime = Server->Date;
	    utime(Queue->DestFile.c_str(),&UBuf);

	    // Send status to APT
	    if (Result == true)
	    {
	       Res.TakeHashes(*Server->In.Hash);
	       URIDone(Res);
	    }
	    else
	    {
	       if (Server->ServerFd == -1)
	       {
		  FailCounter++;
		  _error->Discard();
		  Server->Close();
		  
		  if (FailCounter >= 2)
		  {
		     Fail(_("Connection failed"),true);
		     FailCounter = 0;
		  }
		  
		  QueueBack = Queue;
	       }
	       else
		  Fail(true);
	    }
	    break;
	 }
	 
	 // IMS hit
	 case 1:
	 {
	    URIDone(Res);
	    break;
	 }
	 
	 // Hard server error, not found or something
	 case 3:
	 {
	    Fail();
	    break;
	 }
	  
	 // Hard internal error, kill the connection and fail
	 case 5:
	 {
	    delete File;
	    File = 0;

	    Fail();
	    RotateDNS();
	    Server->Close();
	    break;
	 }

	 // We need to flush the data, the header is like a 404 w/ error text
	 case 4:
	 {
	    Fail();
	    
	    // Send to content to dev/null
	    File = new FileFd("/dev/null",FileFd::WriteExists);
	    Server->RunData();
	    delete File;
	    File = 0;
	    break;
	 }
	 
	 default:
	 Fail(_("Internal error"));
	 break;
      }
      
      FailCounter = 0;
   }
   
   return 0;
}
									/*}}}*/



