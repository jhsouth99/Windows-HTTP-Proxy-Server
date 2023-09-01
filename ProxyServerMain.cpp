#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <tchar.h>

#include <iostream>
#include <cstring>
#include <sstream>
#include <winsock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib" )
#pragma comment(lib, "IPHLPAPI.lib")
#include <Windows.h>

using namespace std;

#define LOCAL_IN_PORT 8088U
ULONG local_out_ip;
int ifIndex;

//function to initialize winsock
bool InitializeWinsock()
{
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		cout << "WSAStartup failed with error: " << iResult << endl;
		return false;
	}
	cout << "WSAStartup successfully initialized." << endl;
	return true;
}

//Function to parse hostname from http request
string ParseHostname(char* buf, int& port)
{
	size_t pos;
	size_t pos2;

	//string to hold hostname substring
	string hostname_t;
	//copy request to string for easier parsing
	string httpheader = buf;

	pos = httpheader.find("Host: ");//find "Host: " line
	hostname_t = httpheader.substr(pos + 6);//copy to substring, not                 including "Host: ", just the hostname
	pos2 = hostname_t.find(":");
	pos = hostname_t.find("\r\n");// find end of line
	if (pos2 < pos) {
		pos = pos2;
		port = stoi(hostname_t.substr(pos2 + 1, pos - pos2 - 1).c_str());
	}
	hostname_t.erase(pos);//erase the rest of the string which is unwanted

	return hostname_t;
}

static inline SOCKADDR_IN socktopeeraddr(SOCKET socket) {
	SOCKADDR_IN socket_info = { 0 };
	int addrsize = sizeof(socket_info);
	getpeername(socket, (sockaddr*)&socket_info, &addrsize);
	return socket_info;
}
static inline SOCKADDR_IN socktolocaddr(SOCKET socket) {
	SOCKADDR_IN socket_info = { 0 };
	int addrsize = sizeof(socket_info);
	getsockname(socket, (sockaddr*)&socket_info, &addrsize);
	return socket_info;
}
static inline string addrtostring(SOCKADDR_IN addr) {
	ostringstream oss;
	oss << inet_ntoa(addr.sin_addr) << ':' << ntohs(addr.sin_port);
	return oss.str();
}
static inline string socktostringpeeraddr(SOCKET socket) {
	SOCKADDR_IN socket_info = socktopeeraddr(socket);
	return addrtostring(socket_info);
}
static inline string socktostringlocaddr(SOCKET socket) {
	SOCKADDR_IN socket_info = socktolocaddr(socket);
	return addrtostring(socket_info);
}

class HttpProxy
{
	SOCKET client_socket;  // Socket we read from
	SOCKET server_socket; // Socket we write to
	char* buffer;         // Buffer we read data into, and write data from
	size_t buffer_size;   // Total size of buffer (allocated memory)
	int read_size;     // Number of bytes read
	ostringstream oss;

public:
	HttpProxy()
		: buffer(0), buffer_size(1500)
	{ }
	void Run(SOCKET sock);
	string GetForwardingInfo() {
		return socktostringpeeraddr(client_socket) + "↔" + socktostringpeeraddr(server_socket);
	}

private:
	string _getsocketsdirection(SOCKET from, SOCKET to) {
		return socktostringpeeraddr(from) + "→" + socktostringpeeraddr(to);
	}
	string _getclientsocketdirection() {
		return socktostringpeeraddr(client_socket) + "→" + socktostringlocaddr(client_socket);
	}
	string _getserversocketdirection() {
		return socktostringlocaddr(server_socket) + "→" + socktostringpeeraddr(server_socket);
	}
	string _getsocketsdirection(sockaddr_in from, sockaddr_in to) {
		return addrtostring(from) + "→" + addrtostring(to);
	}

	void Connect()
	{
		// Read data into the buffer, setting "read_size"
		// Like: read_size = recv(client_socket, buffer_size, 0);
		// Throw exception on error (includes connection closed)
		// NOTE: If error is WSAEWOULDBLOCK, set read_size to 0, don't throw exception

		SOCKADDR_IN local_out;
		read_size = recv(client_socket, buffer, buffer_size, 0);
		if (read_size == 0) {
			oss << _getclientsocketdirection() << " Connect Request recv() failed(client → local) with no data";
			throw exception(oss.str().c_str());
		}
		else if (read_size == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK) {
				read_size = 0;
				return;
			}
			oss << _getclientsocketdirection() << " Connect Request recv() failed(client → local) with error " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}
#ifdef _DEBUG
		oss << _getclientsocketdirection() << " recv() success(client → local). Connect Request Received" << endl;
		cout << oss.str();
		oss.str("");
#endif

		// Connect to the real server
		// Set the server_socket member variable

		SOCKADDR_IN Dest;
		PADDRINFOEX3 pResult = NULL;
		ADDRINFOEX3 hints = { 0, };

		hints.ai_family = PF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_EXTENDED;
		hints.ai_version = ADDRINFOEX_VERSION_3;
		hints.ai_interfaceindex = ifIndex;

		Dest.sin_family = AF_INET;

		// Default server port
		int sin_port = 443;

		//parse hostname from http request
		string hostname = ParseHostname(buffer, sin_port);
		Dest.sin_port = htons(sin_port);
		
		struct addrinfo Hints { 0, };
		Hints.ai_family = PF_INET;

		char servName[8]{ 0, };
		_itoa_s(sin_port, servName, 10);

		int Error = GetAddrInfoEx(hostname.c_str(), servName, NS_ALL, NULL, (const PADDRINFOEX)&hints, (PADDRINFOEX*)&pResult, NULL, NULL, NULL, NULL);

		if (Error != ERROR_SUCCESS) {
			oss << _getclientsocketdirection() << '→' << hostname.c_str() << ':' << sin_port << ' ';
			oss << gai_strerror(Error);
			throw exception(oss.str().c_str());
		}

		Dest.sin_addr = ((PSOCKADDR_IN)pResult->ai_addr)->sin_addr;

		FreeAddrInfoEx((PADDRINFOEX)pResult);

		if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		{
			oss << _getclientsocketdirection() << " socket() failed(local → server) with error " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}

		const BOOL dontroute = TRUE;
		const int dontroute_len = sizeof(dontroute);
		if (setsockopt(server_socket, SOL_SOCKET, SO_DONTROUTE, (char*)&dontroute, dontroute_len)) {
			oss << _getclientsocketdirection() << " setsockopt for SO_DONTROUTE failed(local → server) with error " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}

		local_out.sin_addr.s_addr = local_out_ip;
		local_out.sin_family = AF_INET;
		local_out.sin_port = htons(0U);

		if (bind(server_socket, (LPSOCKADDR)&local_out, sizeof(local_out))) {
			oss << _getsocketsdirection(local_out, Dest) << " bind() failed(local → server) with error: " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}

		// Connect to server
		if (connect(server_socket, (SOCKADDR*)&Dest, sizeof(Dest)) == SOCKET_ERROR)
		{
			oss << _getserversocketdirection() << " connect() failed(local → server): " << _getsocketsdirection(local_out, Dest) << " with error " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}
#ifdef _DEBUG
		oss << _getserversocketdirection() << " connect() success(local → server)" << endl;
		cout << oss.str();
		oss.str("");
#endif

		strcpy_s(buffer, buffer_size, "HTTP/1.1 200 Connection Establisihed\r\n\r\n");
		read_size = strlen(buffer);
		if (send(client_socket, buffer, read_size, MSG_DONTROUTE) == SOCKET_ERROR)
		{
			oss << _getserversocketdirection() << " send() failed(local → server) with error " << WSAGetLastError();
			throw exception(oss.str().c_str());
		}
#ifdef _DEBUG
		oss << _getclientsocketdirection() << " send() success(local → client). Bytes received: " << read_size << endl;
		cout << oss.str();
		oss.str("");
#endif

		oss << "Connected: " << GetForwardingInfo() << endl;
		cout << oss.str();
		oss.str("");

		// Make socket nonblocking
		u_long mode = 1;
		ioctlsocket(server_socket, FIONBIO, &mode);
	}

	void Forward()
	{
		fd_set readSet;
		fd_set writeSet;
		int ret;
		const size_t buffer_size = 1500;
		char* buffer = new char[buffer_size];
		int read_size = 0;
		bool to_continue = true;
		for (; to_continue;) {
			FD_ZERO(&readSet);
			FD_SET(client_socket, &readSet);
			FD_SET(server_socket, &readSet);
			if ((ret = select(0, &readSet, NULL, NULL, NULL)) == SOCKET_ERROR)
			{
				oss << GetForwardingInfo() << " select(fd_read) failed(client → server)";
				throw exception(oss.str().c_str());
			}
			if (ret == 0)
				break;
			if (ret > 0) {
				if (FD_ISSET(client_socket, &readSet)) {
					read_size = recv(client_socket, buffer, buffer_size, 0);
					if (read_size > 0) {
#ifdef _DEBUG
						oss << GetForwardingInfo() << " recv() success(client → server). Bytes received: " << read_size << endl;
						cout << oss.str();
						oss.str("");
#endif
					}
					else if (read_size == 0)
						to_continue = false;
					else if (read_size == SOCKET_ERROR) {
						if (WSAGetLastError() == WSAEWOULDBLOCK) {
							read_size = 0;
							return;
						}
						oss << GetForwardingInfo() << " recv() failed(client → server) with error " << WSAGetLastError();
						throw exception(oss.str().c_str());
					}
					if (read_size > 0)
					{
						FD_ZERO(&writeSet);
						FD_SET(server_socket, &writeSet);
						if ((ret = select(0, NULL, &writeSet, NULL, NULL)) == SOCKET_ERROR)
						{
							oss << GetForwardingInfo() << " select(fd_write) failed(client → server)";
							throw exception(oss.str().c_str());
						}
						if (ret > 0) {
							if (FD_ISSET(server_socket, &writeSet)) {
								// Send data from the buffer
								// Like: send(server_socket, buffer, read_size, 0))
								// Throw exception on error
								if (send(server_socket, buffer, read_size, MSG_DONTROUTE) == SOCKET_ERROR)
								{
									oss << GetForwardingInfo() << " send() failed(client → server) with error " << WSAGetLastError();
									throw exception(oss.str().c_str());
								}
#ifdef _DEBUG
								oss << GetForwardingInfo() << " send() success(client → server). Bytes received: " << read_size << endl;
								cout << oss.str();
								oss.str("");
#endif
							}
						}
					}
				}
				if (FD_ISSET(server_socket, &readSet)) {
					read_size = recv(server_socket, buffer, buffer_size, 0);
					if (read_size > 0) {
#ifdef _DEBUG
						oss << GetForwardingInfo() << " recv() success(server → client). Bytes received: " << read_size << endl;
						cout << oss.str();
						oss.str("");
#endif
					}
					else if (read_size == 0)
						to_continue = false;
					else if (read_size == SOCKET_ERROR) {
						if (WSAGetLastError() == WSAEWOULDBLOCK) {
							read_size = 0;
							return;
						}
						oss << GetForwardingInfo() << " recv() failed(server → client) with error " << WSAGetLastError();
						throw exception(oss.str().c_str());
					}
					if (read_size > 0)
					{
						FD_ZERO(&writeSet);
						FD_SET(client_socket, &writeSet);
						if ((ret = select(0, NULL, &writeSet, NULL, NULL)) == SOCKET_ERROR)
						{
							oss << GetForwardingInfo() << " select() failed(server → client)";
							throw exception(oss.str().c_str());
						}
						if (ret > 0) {
							if (FD_ISSET(client_socket, &writeSet)) {
								// Send data from the buffer
								// Like: send(server_socket, buffer, read_size, 0))
								// Throw exception on error
								if (send(client_socket, buffer, read_size, MSG_DONTROUTE) == SOCKET_ERROR)
								{
									oss << " send() failed(server → client) with error " << WSAGetLastError();
									throw exception(oss.str().c_str());
								}
#ifdef _DEBUG
								oss << GetForwardingInfo() << " send() success(server → client). Bytes received: " << read_size << endl;
								cout << oss.str();
								oss.str("");
#endif
							}
						}
					}
				}
			}
		}
	}
};

void HttpProxy::Run(SOCKET socket)
{
	client_socket = socket;

	if (buffer == 0)
	{
		// Allocate buffer
		buffer = (char*)malloc(buffer_size);
	}

	// Connect to the real server
	try {
		Connect();
		Forward();
	}
	catch (exception& e) {
		cout << e.what() << endl;
		oss.str("");
	}

	// Clean up
	delete[] buffer;
	oss << "Disconnected: " << GetForwardingInfo() << endl;
	cout << oss.str();

	closesocket(server_socket);
	closesocket(client_socket);
}

DWORD proxy_thread(LPVOID param)
{
	SOCKET socket = (SOCKET)param;

	// Make socket nonblocking
	u_long mode = 1;
	ioctlsocket(socket, FIONBIO, &mode);

	// Main thread stuff
	HttpProxy connection;
	connection.Run(socket);
	return 0;
}

void PrintNetworkAdaptersInfo() {
	/* Declare and initialize variables */

	DWORD dwSize = 0;
	DWORD dwRetVal = 0;

	unsigned int i = 0;

	// Set the flags to pass to GetAdaptersAddresses
	ULONG flags = GAA_FLAG_INCLUDE_PREFIX;

	// default to unspecified address family (both)
	ULONG family = AF_UNSPEC;

	LPVOID lpMsgBuf = NULL;

	PIP_ADAPTER_ADDRESSES pAddresses = NULL;
	ULONG outBufLen = 0;
	ULONG Iterations = 0;

	PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
	PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;
	PIP_ADAPTER_ANYCAST_ADDRESS pAnycast = NULL;
	PIP_ADAPTER_MULTICAST_ADDRESS pMulticast = NULL;
	IP_ADAPTER_DNS_SERVER_ADDRESS* pDnServer = NULL;
	IP_ADAPTER_PREFIX* pPrefix = NULL;

	// Allocate a 15 KB buffer to start with.
	outBufLen = 15000;

	do {

		pAddresses = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, outBufLen);
		if (pAddresses == NULL) {
			printf
			("Memory allocation failed for IP_ADAPTER_ADDRESSES struct\n");
			exit(1);
		}

		dwRetVal =
			GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);

		if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
			HeapFree(GetProcessHeap(), 0, pAddresses);
			pAddresses = NULL;
		}
		else {
			break;
		}

		Iterations++;

	} while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (Iterations < 3));

	if (dwRetVal == NO_ERROR) {
		// If successful, output some information from the data we received
		pCurrAddresses = pAddresses;
		while (pCurrAddresses) {
			printf("\tIfIndex (IPv4 interface): %u\n", pCurrAddresses->IfIndex);
			printf("\tDescription: %wS\n", pCurrAddresses->Description);
			printf("\tAdapter name: %s\n", pCurrAddresses->AdapterName);


			if (pCurrAddresses->PhysicalAddressLength != 0) {
				printf("\tPhysical address: ");
				for (i = 0; i < (int)pCurrAddresses->PhysicalAddressLength;
					i++) {
					if (i == (pCurrAddresses->PhysicalAddressLength - 1))
						printf("%.2X\n",
							(int)pCurrAddresses->PhysicalAddress[i]);
					else
						printf("%.2X-",
							(int)pCurrAddresses->PhysicalAddress[i]);
				}
			}
			printf("\tFlags: %ld\n", pCurrAddresses->Flags);
			printf("\tMtu: %lu\n", pCurrAddresses->Mtu);
			printf("\tIfType: %ld\n", pCurrAddresses->IfType);
			printf("\tOperStatus: %ld\n", pCurrAddresses->OperStatus);
			printf("\tIpv6IfIndex (IPv6 interface): %u\n",
				pCurrAddresses->Ipv6IfIndex);


			printf("\n");

			pCurrAddresses = pCurrAddresses->Next;
		}
	}
	else {
		printf("Call to GetAdaptersAddresses failed with error: %d\n",
			dwRetVal);
		if (dwRetVal == ERROR_NO_DATA)
			printf("\tNo addresses were found for the requested parameters\n");
		else {

			if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, dwRetVal, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				// Default language
				(LPTSTR)&lpMsgBuf, 0, NULL)) {
				printf("\tError: %s", lpMsgBuf);
				LocalFree(lpMsgBuf);
				if (pAddresses)
					HeapFree(GetProcessHeap(), 0, pAddresses);
				exit(1);
			}
		}
	}

	if (pAddresses) {
		HeapFree(GetProcessHeap(), 0, pAddresses);
	}
}

ULONG GetAddressByAdapter(int _IfIdx) {
	/* Declare and initialize variables */

	int i;
	ULONG ret = 0;
	int numEntries;

	/* Variables used by GetIpAddrTable */
	PMIB_IPADDRTABLE pIPAddrTable;
	DWORD dwSize = 0;
	DWORD dwRetVal = 0;
	IN_ADDR IPAddr;

	/* Variables used to return error message */
	LPVOID lpMsgBuf;

	// Before calling AddIPAddress we use GetIpAddrTable to get
	// an adapter to which we can add the IP.
	pIPAddrTable = (MIB_IPADDRTABLE*)HeapAlloc(GetProcessHeap(), 0, sizeof(MIB_IPADDRTABLE));

	if (pIPAddrTable) {
		// Make an initial call to GetIpAddrTable to get the
		// necessary size into the dwSize variable
		if (GetIpAddrTable(pIPAddrTable, &dwSize, 0) ==
			ERROR_INSUFFICIENT_BUFFER) {
			HeapFree(GetProcessHeap(), 0, pIPAddrTable);
			pIPAddrTable = (MIB_IPADDRTABLE*)HeapAlloc(GetProcessHeap(), 0, dwSize);
		}
		if (pIPAddrTable == NULL) {
			printf("Memory allocation failed for GetIpAddrTable\n");
			exit(EXIT_FAILURE);
		}
	}
	// Make a second call to GetIpAddrTable to get the
	// actual data we want
	if ((dwRetVal = GetIpAddrTable(pIPAddrTable, &dwSize, 0)) != NO_ERROR) {
		printf("GetIpAddrTable failed with error %d\n", dwRetVal);
		if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dwRetVal, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),       // Default language
			(LPTSTR)&lpMsgBuf, 0, NULL)) {
			printf("\tError: %s\n", (LPTSTR)lpMsgBuf);
			LocalFree(lpMsgBuf);
		}
		exit(EXIT_FAILURE);
	}

	numEntries = pIPAddrTable->dwNumEntries;
	printf("\tNum Entries: %ld\n", numEntries);
	for (i = 0; i < numEntries; i++) {
		if (pIPAddrTable->table[i].dwIndex == _IfIdx) {
			ret = pIPAddrTable->table[i].dwAddr;
			break;
		}
	}

	if (pIPAddrTable) {
		HeapFree(GetProcessHeap(), 0, pIPAddrTable);
		pIPAddrTable = NULL;
	}

	if (i == numEntries)
		throw exception("No such interface index exists");
	else
		return ret;
}

ULONG GetAdapterByName(char* _Name) {
	/* Declare and initialize variables */

	DWORD dwSize = 0;
	DWORD dwRetVal = 0;

	unsigned int i = 0;

	// Set the flags to pass to GetAdaptersAddresses
	ULONG flags = GAA_FLAG_INCLUDE_PREFIX;

	// default to unspecified address family (both)
	ULONG family = AF_UNSPEC;

	LPVOID lpMsgBuf = NULL;

	PIP_ADAPTER_ADDRESSES pAddresses = NULL;
	ULONG outBufLen = 0;
	ULONG Iterations = 0;

	PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
	PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;
	PIP_ADAPTER_ANYCAST_ADDRESS pAnycast = NULL;
	PIP_ADAPTER_MULTICAST_ADDRESS pMulticast = NULL;
	IP_ADAPTER_DNS_SERVER_ADDRESS* pDnServer = NULL;
	IP_ADAPTER_PREFIX* pPrefix = NULL;

	// Allocate a 15 KB buffer to start with.
	outBufLen = 15000;

	do {

		pAddresses = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, outBufLen);
		if (pAddresses == NULL) {
			printf
			("Memory allocation failed for IP_ADAPTER_ADDRESSES struct\n");
			exit(1);
		}

		dwRetVal =
			GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);

		if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
			HeapFree(GetProcessHeap(), 0, pAddresses);
			pAddresses = NULL;
		}
		else {
			break;
		}

		Iterations++;

	} while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (Iterations < 3));

	ULONG _ifindex = 0UL;
	int strLen = strlen(_Name);
	if (dwRetVal == NO_ERROR) {
		// If successful, output some information from the data we received
		pCurrAddresses = pAddresses;
		while (pCurrAddresses) {
			//char cTemp[256];
			//WideCharToMultiByte(CP_ACP, 0, , strLen, cTemp, strLen, NULL, NULL);
			
			if (!strcmp(pCurrAddresses->AdapterName, _Name)) {
				_ifindex = pCurrAddresses->IfIndex;
					break;
			}

			pCurrAddresses = pCurrAddresses->Next;
		}
	}
	else {
		printf("Call to GetAdaptersAddresses failed with error: %d\n",
			dwRetVal);
		if (dwRetVal == ERROR_NO_DATA)
			printf("\tNo addresses were found for the requested parameters\n");
		else {

			if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, dwRetVal, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				// Default language
				(LPTSTR)&lpMsgBuf, 0, NULL)) {
				printf("\tError: %s", lpMsgBuf);
				LocalFree(lpMsgBuf);
				if (pAddresses)
					HeapFree(GetProcessHeap(), 0, pAddresses);
				exit(1);
			}
		}
	}

	if (pAddresses) {
		HeapFree(GetProcessHeap(), 0, pAddresses);
	}
	return _ifindex;
}

int _tmain(int argc, _TCHAR* argv[])
{
	if (!InitializeWinsock()) {
		return EXIT_FAILURE;
	}
	SOCKADDR_IN local_in;
	SOCKET listen_socket = 0;
	ostringstream oss;

	memset(&local_in, 0, sizeof(local_in));

	local_in.sin_family = AF_INET;
	local_in.sin_port = htons(LOCAL_IN_PORT);

	if (argc == 1) {
		PrintNetworkAdaptersInfo();
		cin >> ifIndex;
		local_in.sin_addr.s_addr = 0x0100007FUL;
	}
	else if (argc == 2) {
		if (argv[1][0] == '-') {
			local_in.sin_addr.s_addr = 0x0100007FUL;
			ifIndex = stoi(argv[1] + 1U);
		}
		else {
			local_in.sin_addr.s_addr = 0x0100007FUL;
			ifIndex = GetAdapterByName(argv[1]);
		}
	}
	else {
		return EXIT_FAILURE;
	}

	try {
		local_out_ip = GetAddressByAdapter(ifIndex);
	}
	catch (exception e) {
		cout << e.what() << endl;
		WSACleanup();
		return EXIT_FAILURE;
	}
	listen_socket = socket(AF_INET, SOCK_STREAM, 0);

	//create socket for listening to
	if (bind(listen_socket, (LPSOCKADDR)&local_in, sizeof(local_in)) == 0)
	{
		if (listen(listen_socket, 10) == 0)
		{
			cout << "Listening on: " << socktostringlocaddr(listen_socket) << endl;
		}
		else
		{
			cout << "Error listening on socket.\n";
			closesocket(listen_socket);
			WSACleanup();
			return EXIT_FAILURE;
		}
	}
	else {
		cerr << "bind() failed(client → local) with error " << WSAGetLastError() << endl;
		closesocket(listen_socket);
		WSACleanup();
		return EXIT_FAILURE;
	}
	SOCKADDR_IN csin{};
	int csin_len = sizeof(csin);
	int iResult = 0;


	//bind function associates a local address with a socket.
	for (;;)
	{
		//accept client connection
		SOCKET client_socket = accept(listen_socket, (LPSOCKADDR)&csin, &csin_len);
		if (client_socket == INVALID_SOCKET) {
			oss << socktostringpeeraddr(listen_socket) << "→" << socktostringlocaddr(listen_socket) << " accept() failed with error " << WSAGetLastError() << endl;
			cout << oss.str();
			closesocket(client_socket);
			closesocket(listen_socket);
			WSACleanup();
			return EXIT_FAILURE;
		}
#ifdef _DEBUG
		oss << socktostringpeeraddr(client_socket) << "→" << socktostringlocaddr(client_socket) << " connect() success(client → local)" << endl;
		cout << oss.str();
		oss.str("");
#endif
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)proxy_thread, (LPVOID)client_socket, 0, 0);
	}
	WSACleanup();
	return EXIT_SUCCESS;
}
