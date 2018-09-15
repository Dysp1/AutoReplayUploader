#include "AutoReplayUploaderPlugin.h"
#include "bakkesmod/wrappers/GameEvent/ReplayWrapper.h"
#include "utils/io.h"
#include <sstream>
#include <windows.h>

BAKKESMOD_PLUGIN(AutoReplayUploaderPlugin, "Auto replay uploader plugin", "0.1", 0)

HTTPRequestHandle hdl;

AutoReplayUploaderPlugin::AutoReplayUploaderPlugin()
{
	std::stringstream userAgentStream;
	userAgentStream << exports.pluginName << "/" << exports.pluginVersion << " BakkesModAPI/" << BAKKESMOD_PLUGIN_API_VERSION;
	userAgent = userAgentStream.str();
}

void AutoReplayUploaderPlugin::onLoad()
{
	auto steamApi = GetModuleHandle("steam_api.dll");

	steamHTTPInstance = (ISteamHTTP*)((uintptr_t(__cdecl*)(void))GetProcAddress(steamApi, "SteamHTTP"))();
	
	SteamAPI_RunCallbacks_Function = (SteamAPI_RunCallbacks_typedef)(GetProcAddress(steamApi, "SteamAPI_RunCallbacks"));
	SteamAPI_RegisterCallResult_Function = (SteamAPI_RegisterCallResult_typedef)(GetProcAddress(steamApi, "SteamAPI_RegisterCallResult"));
	SteamAPI_UnregisterCallResult_Function = (SteamAPI_RegisterCallResult_typedef)(GetProcAddress(steamApi, "SteamAPI_UnregisterCallResult"));

	gameWrapper->HookEventWithCaller<ServerWrapper>("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded", 
		std::bind(&AutoReplayUploaderPlugin::OnGameComplete, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

	cvarManager->registerCvar("cl_autoreplayupload_filepath", "./bakkesmod/data/autoreplaysave.replay", "Path to save to be uploaded replay to.").bindTo(savedReplayPath);
	cvarManager->registerCvar("cl_autoreplayupload_calculated", "1", "Upload to replays to calculated.gg automatically", true, true, 0, true, 1).bindTo(uploadToCalculated);

	//cvarManager->registerNotifier("ultest", [this](std::vector<string> params)
	//{
	//	UploadToCalculated("test.replay");
	//}, "", PERMISSION_ALL);
}

void AutoReplayUploaderPlugin::onUnload()
{
}

void AutoReplayUploaderPlugin::OnGameComplete(ServerWrapper caller, void * params, std::string eventName)
{
	if (!*uploadToCalculated)
	{
		return; //Not uploading replays
	}
	ReplayDirectorWrapper replayDirector = caller.GetReplayDirector();
	if (replayDirector.IsNull())
	{
		cvarManager->log("Could not upload replay, director is NULL!");
		return;
	}
	ReplaySoccarWrapper soccarReplay = replayDirector.GetReplay();
	if (soccarReplay.memory_address == NULL)
	{
		cvarManager->log("Could not upload replay, replay is NULL!");
		return;
	}
	if (file_exists(*savedReplayPath))
	{
		remove((*savedReplayPath).c_str());
	}
	soccarReplay.ExportReplay(*savedReplayPath);

	UploadToCalculated(*savedReplayPath);

}
std::vector<uint8> postData;
void AutoReplayUploaderPlugin::UploadToCalculated(std::string filename)
{
	std::ifstream replayFile(filename, std::ios::binary | std::ios::ate);
	std::streamsize replayFileSize = replayFile.tellg();
	replayFile.seekg(0, std::ios::beg);
	cvarManager->log("Replay size: " + to_string(replayFileSize));
	std::vector<uint8> data(replayFileSize, 0);
	replayFile.read(reinterpret_cast<char*>(&data[0]), replayFileSize);
	cvarManager->log("data size: " + to_string(data.size()));
	replayFile.close();

	HTTPRequestHandle hdl;
	hdl = steamHTTPInstance->CreateHTTPRequest(k_EHTTPMethodPOST, CALCULATED_ENDPOINT);
	SteamAPICall_t* callHandle = NULL;
	steamHTTPInstance->SetHTTPRequestHeaderValue(hdl, "User-Agent", userAgent.c_str());

	std::stringstream postBody;
	postBody << "--" << UPLOAD_BOUNDARY << "\r\n";
	postBody << "Content-Disposition: form-data; name=\"file\"; filename=\"autosavedreplay.replay\"" << "\r\n";
	postBody << "Content-Type: application/octet-stream" << "\r\n";
	postBody << "\r\n";
	postBody << std::string(data.begin(), data.end());
	postBody << "\r\n";
	postBody << "--" << UPLOAD_BOUNDARY << "--" << std::endl;
	
	auto postBodyString = postBody.str();
	postData = std::vector<uint8>(postBodyString.begin(), postBodyString.end());

	std::stringstream contentType;
	contentType << "multipart/form-data;boundary=" << UPLOAD_BOUNDARY << "";
	steamHTTPInstance->SetHTTPRequestHeaderValue(hdl, "Content-Length", to_string(postData.size()).c_str());

	if (!steamHTTPInstance->SetHTTPRequestRawPostBody(hdl, contentType.str().c_str(), &postData[0], postData.size()))
	{
		cvarManager->log("Could not set post body!");
		steamHTTPInstance->ReleaseHTTPRequest(hdl);
		return;
	}
	cvarManager->log("Body size: " + to_string(postData.size()));
	FileUploadData* uploadData = new FileUploadData();
	uploadData->requestHandle = hdl;
	steamHTTPInstance->SendHTTPRequest(uploadData->requestHandle, &uploadData->apiCall);
	uploadData->requestCompleteCallback.Set(uploadData->apiCall, uploadData, &FileUploadData::OnRequestComplete);

	fileUploadsInProgress.push_back(uploadData);
	CheckFileUploadProgress(gameWrapper.get());
}

void AutoReplayUploaderPlugin::CheckFileUploadProgress(GameWrapper * gw)
{
	cvarManager->log("Running callback, size: " + to_string(fileUploadsInProgress.size()));
	SteamAPI_RunCallbacks_Function();
	cvarManager->log("Ran callback");
	for (auto it = fileUploadsInProgress.begin(); it != fileUploadsInProgress.end();)
	{
		if ((*it)->canBeDeleted)
		{
			steamHTTPInstance->ReleaseHTTPRequest((*it)->requestHandle);
			delete (*it);
			cvarManager->log("Erased request");
			it = fileUploadsInProgress.erase(it);
		}
		else
		{
			it++;
		}
	}
	if (!fileUploadsInProgress.empty())
	{
		gw->SetTimeout(std::bind(&AutoReplayUploaderPlugin::CheckFileUploadProgress, this, std::placeholders::_1), .5f);
	}
}
