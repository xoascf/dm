#include "NetworkerThread.hpp"
#include "WinUtils.hpp"
#include "../discord/DiscordRequest.hpp"
#include "../discord/Frontend.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT

#ifndef __MINGW32__
#define __MINGW32__ // so that it doesn't use inet_pton
#endif

#include <httplib/httplib.h>

extern HWND g_Hwnd;

int NetRequest::Priority() const
{
	int prio = 0;

	switch (type)
	{
		case QUIT:
			prio = 200;
			break;
		case PUT:
		case POST:
		case POST_JSON:
		case PATCH:
			prio = 100;
			break;
		case GET:
			prio =  90;
			break;
		default:
			assert(!"huh?");
	}

	switch (itype) {
		using namespace DiscordRequest;
		default:
			prio += 9;
			break;

		case IMAGE_ATTACHMENT:
		case MESSAGES:
		case GUILD:
			prio += 8;
			break;

		case IMAGE:
			prio += 1;
			break;
	}

	return prio;
}

void NetworkerThread::ProcessResult(NetRequest& req, const httplib::Result& res)
{
	using namespace httplib;

	if (!res)
	{
		req.result = -1;
		req.response = to_string(res.error());
	}
	else
	{
		req.result = res->status;
		if (res->status == HTTP_OK)
			req.response = res->body;
		else
			req.response = std::string(detail::status_message(res->status));
	}

	// Call the handler function.
	// N.B.  Don't return unless you're absolutely done with the request!
	req.pFunc(&req);
}

void NetworkerThread::IdleWait()
{
	Sleep(100);
}

void NetworkerThread::FulfillRequest(NetRequest& req)
{
	std::string& url = req.url;

	// split the URL into its host name and path
	std::string hostName = "", path = "";
	auto pos = url.find("://"), pos2 = pos;
	if (pos != std::string::npos)
		pos2 = url.find("/", pos + 4);
	else
		pos2 = url.find("/");

	if (pos2 != std::string::npos)
	{
		hostName = url.substr(0, pos2);
		path = url.substr(pos2);
	}

	using namespace httplib;
	Client client(hostName);

	// on Windows XP, enabling this doesn't actually work for some reason.
	// Probably outdated certs. I mean, this would allow attackers to host
	// a self-instance of Discord to intercept packets, but this is fine
	// for now.....
	client.enable_server_certificate_verification(false);

	Headers headers;
	headers.insert(std::make_pair("User-Agent", GetFrontend()->GetUserAgent()));

	if (req.authorization.size())
	{
		headers.insert(std::make_pair("Authorization", req.authorization));
	}

	switch (req.type)
	{
		// no default constructor for httplib::Result?? this SUCKS!
		case NetRequest::POST:
		{
			const Result res = client.Post(path, headers, req.params, "application/x-www-form-urlencoded");
			ProcessResult(req, res);
			break;
		}
		case NetRequest::POST_JSON:
		{
			const Result res = client.Post(path, headers, req.params, "application/json");
			ProcessResult(req, res);
			break;
		}
		case NetRequest::PUT:
		{
			const Result res = client.Put(path, headers, req.params, "application/x-www-form-urlencoded");
			ProcessResult(req, res);
			break;
		}
		case NetRequest::PUT_OCTETS:
		{
			const Result res = client.Put(path, headers, (const char*) req.params_bytes.data(), req.params_bytes.size(), "application/octet-stream");
			ProcessResult(req, res);
			break;
		}
		case NetRequest::GET:
		{
			const Result res = client.Get(path, headers);
			ProcessResult(req, res);
			break;
		}
		case NetRequest::PATCH:
		{
			const Result res = client.Patch(path, headers, req.params, "application/json");
			ProcessResult(req, res);
			break;
		}
		case NetRequest::DELETE_:
		{
			const Result res = client.Delete(path, headers, req.params, "application/json");
			ProcessResult(req, res);
			break;
		}
		default:
			assert(!"Don't know how to handle that type of request!");
			break;
	}
}

void NetworkerThread::Run()
{
	while (true)
	{
		// lock the mutex
		m_requestLock.lock();
		if (m_requests.empty())
		{
			m_requestLock.unlock();
			IdleWait();
			continue;
		}

		NetRequest request = std::move(m_requests.top());
		m_requests.pop();
		m_requestLock.unlock();

		// Service the request.
		if (request.type == NetRequest::QUIT)
			break;

		FulfillRequest(request);
	}

	m_ThreadHandle = NULL;
	m_ThreadID = 0;
}

DWORD WINAPI NetworkerThread::Init(LPVOID that)
{
	NetworkerThread* pThrd = (NetworkerThread*)that;
	pThrd->Run();
	return 0;
}

void OutputPrintf(const char* str, ...);

void NetworkerThread::AddRequest(
	NetRequest::eType type,
	const std::string& url,
	int itype,
	uint64_t requestKey,
	std::string params,
	std::string authorization,
	std::string additional_data,
	NetRequest::NetworkResponseFunc pRespFunc,
	uint8_t* stream_bytes,
	size_t stream_size)
{
	NetRequest rq(0, itype, requestKey, type, url, "", params, authorization, additional_data, pRespFunc, stream_bytes, stream_size);

	m_requestLock.lock();
	m_requests.push(rq);
	m_requestLock.unlock();
}

void NetworkerThread::StopAllRequests()
{
	m_requestLock.lock();
	while (!m_requests.empty())
		m_requests.pop();
	m_requestLock.unlock();
}

void NetworkerThread::PrepareQuit()
{
	m_requestLock.lock();

	while (!m_requests.empty())
		m_requests.pop();

	NetRequest rq(0, 0, 0, NetRequest::QUIT);
	m_requests.push(rq);

	m_requestLock.unlock();
}

NetworkerThread::NetworkerThread()
{
	m_ThreadHandle = CreateThread(
		NULL,
		0,
		Init,
		this,
		0,
		&m_ThreadID
	);

	if (!m_ThreadHandle)
	{
		HRESULT hr = GetLastError();
		std::string str = "Could not start NetworkerThread. Discord Messenger will now close.\n\n(" + std::to_string(hr) + ") " + GetStringFromHResult(hr);
		LPCTSTR ctstr = ConvertCppStringToTString(str);
		MessageBox(g_Hwnd, ctstr, TEXT("Discord Messenger - Fatal Error"), MB_ICONERROR | MB_OK);
		free((void*)ctstr);
		exit(1);
	}
}

NetworkerThread::~NetworkerThread()
{
	PrepareQuit();

	// wait for the thread to go away
	WaitForSingleObject(m_ThreadHandle, INFINITE);
}

NetworkerThreadManager::~NetworkerThreadManager()
{
	assert(m_bKilled && "Ideally you wouldn't kill now");
	Kill();
}

void NetworkerThreadManager::Init()
{
	m_bKilled = false;
	for (int i = 0; i < C_AMT_NETWORKER_THREADS; i++)
		m_pNetworkThreads[i] = new NetworkerThread();
}

void NetworkerThreadManager::StopAllRequests()
{
	for (int i = 0; i < C_AMT_NETWORKER_THREADS; i++) {
		if (m_pNetworkThreads[i])
			m_pNetworkThreads[i]->StopAllRequests();
	}
}

void NetworkerThreadManager::PrepareQuit()
{
	for (int i = 0; i < C_AMT_NETWORKER_THREADS; i++) {
		if (m_pNetworkThreads[i])
			m_pNetworkThreads[i]->PrepareQuit();
	}
}

void NetworkerThreadManager::Kill()
{
	PrepareQuit();

	// Wait for all networker threads to quit
	HANDLE threads[C_AMT_NETWORKER_THREADS];
	int nthreads = 0;

	for (int i = 0; i < C_AMT_NETWORKER_THREADS; i++)
	{
		if (!m_pNetworkThreads[i])
			continue;

		HANDLE h = m_pNetworkThreads[i]->GetThreadHandle();
		if (h)
			threads[nthreads++] = h;
	}

	if (nthreads > 0)
		WaitForMultipleObjects(nthreads, threads, true, INFINITE);

	for (int i = 0; i < C_AMT_NETWORKER_THREADS; i++)
	{
		if (m_pNetworkThreads[i])
			delete m_pNetworkThreads[i];

		m_pNetworkThreads[i] = NULL;
	}

	m_bKilled = true;
}

void NetworkerThreadManager::PerformRequest(
	bool interactive,
	NetRequest::eType type,
	const std::string& url,
	int itype,
	uint64_t requestKey,
	std::string params,
	std::string authorization,
	std::string additional_data,
	NetRequest::NetworkResponseFunc pRespFunc,
	uint8_t* stream_bytes,
	size_t stream_size)
{
	//OutputPrintf("*** PERFORM REQUEST (%s) ***", url.c_str());
	//OutputPrintf("Method: %d", type);
	//OutputPrintf("IType: %d", itype);
	//OutputPrintf("Key: %llu", requestKey);
	//OutputPrintf("AdditionalData: %s", additional_data.c_str());
	//OutputPrintf("StreamSize: %zu", stream_size);
	//OutputPrintf("Body: <START>%s<END>", params.c_str());

	int idx;
	if (interactive) {
		m_nextInteractiveId = (m_nextInteractiveId + 1) % C_INTERACTIVE_NETWORKER_THREADS;
		idx = m_nextInteractiveId;
	}
	else {
		m_nextBackgroundId = C_INTERACTIVE_NETWORKER_THREADS + (m_nextBackgroundId + 1) % (C_AMT_NETWORKER_THREADS - C_INTERACTIVE_NETWORKER_THREADS);
		idx = m_nextBackgroundId;
	}

	m_pNetworkThreads[idx]->AddRequest(type, url, itype, requestKey, params, authorization, additional_data, pRespFunc, stream_bytes, stream_size);
}
