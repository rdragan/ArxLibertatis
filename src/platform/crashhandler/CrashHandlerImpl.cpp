/*
 * Copyright 2011 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "CrashHandlerImpl.h"

#include "io/fs/Filesystem.h"
#include "io/fs/FilePath.h"

CrashHandlerImpl::CrashHandlerImpl()
	: m_pCrashInfo(0) {
}

CrashHandlerImpl::~CrashHandlerImpl() {
}

bool CrashHandlerImpl::initialize() {
	Autolock autoLock(&m_Lock);

	bool initialized = true;
	
	bool crashReporterFound = fs::exists(m_CrashHandlerApp);
	if(crashReporterFound) {
		LogInfo << "CrashReporter found, initializing crash handler.";
	} else {
		LogError << "CrashReporter not found, disabling crash handler.";
		return false;
	}

	if(!createSharedMemory()) {
		LogError << "Failed to create shared memory.";
		return false;
	}

	fillBasicCrashInfo();

	if(!registerCrashHandlers()) {
		LogError << "Failed to register crash handlers.";
		destroySharedMemory();
		initialized = false;
	}

	return initialized;
}

void CrashHandlerImpl::shutdown() {
	Autolock autoLock(&m_Lock);
	
	unregisterCrashHandlers();
	destroySharedMemory();
}

bool CrashHandlerImpl::createSharedMemory() {
	// Generate a random name for our shared memory object
	boost::uuids::uuid uid = boost::uuids::random_generator()();
	m_SharedMemoryName = boost::lexical_cast<std::string>(uid);

	// Create a shared memory object.
	m_SharedMemory = boost::interprocess::shared_memory_object(boost::interprocess::create_only, m_SharedMemoryName.c_str(), boost::interprocess::read_write);

	// Resize to fit the CrashInfo structure
	m_SharedMemory.truncate(sizeof(CrashInfo));

	// Map the whole shared memory in this process
	m_MemoryMappedRegion = boost::interprocess::mapped_region(m_SharedMemory, boost::interprocess::read_write);

	// Our CrashInfo will be stored in this shared memory.
	m_pCrashInfo = new (m_MemoryMappedRegion.get_address()) CrashInfo;

	return true;
}

void CrashHandlerImpl::destroySharedMemory() {
	m_MemoryMappedRegion = boost::interprocess::mapped_region();
	m_SharedMemory = boost::interprocess::shared_memory_object();
	m_pCrashInfo = 0;
}

void CrashHandlerImpl::fillBasicCrashInfo() {
	m_pCrashInfo->processId = boost::interprocess::detail::get_current_process_id();

	strcpy(m_pCrashInfo->crashReportFolder, "crashes");
}

bool CrashHandlerImpl::addAttachedFile(const fs::path& file) {
	Autolock autoLock(&m_Lock);

	if(m_pCrashInfo->nbFilesAttached == CrashInfo::MaxNbFiles) {
		LogError << "Too much files already attached to the crash report (" << m_pCrashInfo->nbFilesAttached << ").";
		return false;
	}

	if(file.string().size() >= CrashInfo::MaxFilenameLen) {
		LogError << "File name is too long.";
		return false;
	}

	for(int i = 0; i < m_pCrashInfo->nbFilesAttached; i++) {
		if(strcmp(m_pCrashInfo->attachedFiles[i], file.string().c_str()) == 0) {
			LogWarning << "File \"" << file << "\" is already attached.";
			return false;
		}
	}

	strcpy(m_pCrashInfo->attachedFiles[m_pCrashInfo->nbFilesAttached], file.string().c_str());
	m_pCrashInfo->nbFilesAttached++;

	return true;
}

bool CrashHandlerImpl::setNamedVariable(const std::string& name, const std::string& value) {
	Autolock autoLock(&m_Lock);

	if(name.size() >= CrashInfo::MaxVariableNameLen) {
		LogError << "Variable name is too long.";
		return false;
	}

	if(value.size() >= CrashInfo::MaxVariableValueLen) {
		LogError << "Variable description is too long.";
		return false;
	}

	// Check if our array already contains this variable.
	for(int i = 0; i < m_pCrashInfo->nbVariables; i++) {
		if(strcmp(m_pCrashInfo->variables[i].name, name.c_str()) == 0) {
			strcpy(m_pCrashInfo->variables[i].value, value.c_str());
			return true;
		}
	}

	// Not found, must add a new one.
	if(m_pCrashInfo->nbVariables == CrashInfo::MaxNbVariables) {
		LogError << "Too much variables already added to the crash report (" << m_pCrashInfo->nbVariables << ").";
		return false;
	}

	strcpy(m_pCrashInfo->variables[m_pCrashInfo->nbVariables].name, name.c_str());
	strcpy(m_pCrashInfo->variables[m_pCrashInfo->nbVariables].value, value.c_str());
	m_pCrashInfo->nbVariables++;

	return true;
}

bool CrashHandlerImpl::setReportLocation(const fs::path& location) {
	Autolock autoLock(&m_Lock);

	if(location.string().size() >= CrashInfo::MaxFilenameLen) {
		LogError << "Report location path is too long.";
		return false;
	}

	strcpy(m_pCrashInfo->crashReportFolder, location.string().c_str());

	return true;
}

bool CrashHandlerImpl::deleteOldReports(size_t nbReportsToKeep)
{
	Autolock autoLock(&m_Lock);

    if(strlen(m_pCrashInfo->crashReportFolder) == 0) {
		LogError << "Report location has not been specified";
		return false;
	}
	
	// Exit if there is no crash report folder yet...
	fs::path location(m_pCrashInfo->crashReportFolder);
	if(!fs::is_directory(location))
		return true;

	typedef std::map<std::string, fs::path> CrashReportMap;
	CrashReportMap oldCrashes;

	for(fs::directory_iterator it(location); !it.end(); ++it) {
		fs::path folderPath = location / it.name();
		if(fs::is_directory(folderPath)) {
			// Ensure this directory really contains a crash report...
			fs::path crashManifestPath = folderPath / "crash.xml";
			if(fs::is_regular_file(crashManifestPath)) {
				oldCrashes[it.name()] = folderPath;
			}
		}
	}

	// Nothing to delete
	if(nbReportsToKeep >= oldCrashes.size())
		return true;

	int nbReportsToDelete = oldCrashes.size() - nbReportsToKeep; 

	// std::map will return the oldest reports first as folders are named "yyyy.MM.dd hh.mm.ss"
	CrashReportMap::const_iterator it = oldCrashes.begin();
	for(int i = 0; i < nbReportsToDelete; ++i, ++it) {
		fs::remove_all(it->second);
	}
	
	return true;
}

void CrashHandlerImpl::registerCrashCallback(CrashHandler::CrashCallback crashCallback) {
	Autolock autoLock(&m_Lock);

	m_crashCallbacks.push_back(crashCallback);
}

void CrashHandlerImpl::unregisterCrashCallback(CrashHandler::CrashCallback crashCallback) {
	Autolock autoLock(&m_Lock);

	m_crashCallbacks.erase(std::remove(m_crashCallbacks.begin(),
	                                   m_crashCallbacks.end(),
	                                   crashCallback),
	                       m_crashCallbacks.end());
}