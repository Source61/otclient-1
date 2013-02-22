/*
 * Copyright (c) 2010-2013 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "resourcemanager.h"
#include "filestream.h"

#include <framework/core/application.h>
#include <framework/luaengine/luainterface.h>
#include <framework/platform/platform.h>

#include <physfs.h>

ResourceManager g_resources;

void ResourceManager::init(const char *argv0)
{
    PHYSFS_init(argv0);
    PHYSFS_permitSymbolicLinks(1);
}

void ResourceManager::terminate()
{
    PHYSFS_deinit();
}

bool ResourceManager::discoverWorkDir(const std::string& existentFile)
{
    // search for modules directory
    std::string possiblePaths[] = { g_resources.getCurrentDir(),
                                    g_resources.getBaseDir(),
                                    g_resources.getBaseDir() + "../",
                                    g_resources.getBaseDir() + "../share/" + g_app.getCompactName() + "/" };
    bool found = false;
    for(const std::string& dir : possiblePaths) {
        if(!PHYSFS_addToSearchPath(dir.c_str(), 0))
            continue;

        if(PHYSFS_exists(existentFile.c_str())) {
            g_logger.debug(stdext::format("Found work dir at '%s'", dir));
            m_workDir = dir;
            found = true;
            break;
        }
        PHYSFS_removeFromSearchPath(dir.c_str());
    }

    return found;
}

bool ResourceManager::setupUserWriteDir(const std::string& appWriteDirName)
{
    std::string userDir = getUserDir();
    std::string dirName;
#ifndef WIN32
    dirName = stdext::format(".%s", appWriteDirName);
#else
    dirName = appWriteDirName;
#endif
    std::string writeDir = userDir + dirName;

    if(!PHYSFS_setWriteDir(writeDir.c_str())) {
        if(!PHYSFS_setWriteDir(userDir.c_str()) || !PHYSFS_mkdir(dirName.c_str())) {
            g_logger.error(stdext::format("Unable to create write directory '%s': %s", writeDir, PHYSFS_getLastError()));
            return false;
        }
    }
    return setWriteDir(writeDir);
}

bool ResourceManager::setWriteDir(const std::string& writeDir, bool create)
{
    if(!PHYSFS_setWriteDir(writeDir.c_str())) {
        g_logger.error(stdext::format("Unable to set write directory '%s': %s", writeDir, PHYSFS_getLastError()));
        return false;
    }

    if(!m_writeDir.empty())
        removeSearchPath(m_writeDir);

    m_writeDir = writeDir;

    if(!addSearchPath(writeDir))
        g_logger.error(stdext::format("Unable to add write '%s' directory to search path", writeDir));

    return true;
}

bool ResourceManager::addSearchPath(const std::string& path, bool pushFront)
{
    std::string savePath = path;
    if(!PHYSFS_addToSearchPath(path.c_str(), pushFront ? 0 : 1)) {
        bool found = false;
        for(std::string searchPath : m_searchPaths) {
            std::string newPath = searchPath + path;
            if(PHYSFS_addToSearchPath(newPath.c_str(), pushFront ? 0 : 1)) {
                savePath = newPath;
                found = true;
                break;
            }
        }

        if(!found) {
            //g_logger.error(stdext::format("Could not add '%s' to directory search path. Reason %s", path, PHYSFS_getLastError()));
            return false;
        }
    }
    if(pushFront)
        m_searchPaths.push_front(savePath);
    else
        m_searchPaths.push_back(savePath);
    return true;
}

bool ResourceManager::removeSearchPath(const std::string& path)
{
    if(!PHYSFS_removeFromSearchPath(path.c_str()))
        return false;
    auto it = std::find(m_searchPaths.begin(), m_searchPaths.end(), path);
    assert(it != m_searchPaths.end());
    m_searchPaths.erase(it);
    return true;
}

void ResourceManager::searchAndAddPackages(const std::string& packagesDir, const std::string& packageExt)
{
    auto files = listDirectoryFiles(packagesDir);
    for(auto it = files.rbegin(); it != files.rend(); ++it) {
        const std::string& file = *it;
        if(!stdext::ends_with(file, packageExt))
            continue;
        std::string package = getRealDir(packagesDir) + "/" + file;
        if(!addSearchPath(package, true))
            g_logger.error(stdext::format("Unable to read package '%s': %s", package, PHYSFS_getLastError()));
    }
}

bool ResourceManager::fileExists(const std::string& fileName)
{
    return (PHYSFS_exists(resolvePath(fileName).c_str()) && !PHYSFS_isDirectory(resolvePath(fileName).c_str()));
}

bool ResourceManager::directoryExists(const std::string& directoryName)
{
    return (PHYSFS_isDirectory(resolvePath(directoryName).c_str()));
}

void ResourceManager::readFileStream(const std::string& fileName, std::iostream& out)
{
    std::string buffer = readFileContents(fileName);
    if(buffer.length() == 0) {
        out.clear(std::ios::eofbit);
        return;
    }
    out.clear(std::ios::goodbit);
    out.write(&buffer[0], buffer.length());
    out.seekg(0, std::ios::beg);
}

std::string ResourceManager::readFileContents(const std::string& fileName)
{
    std::string fullPath = resolvePath(fileName);

    PHYSFS_File* file = PHYSFS_openRead(fullPath.c_str());
    if(!file)
        stdext::throw_exception(stdext::format("unable to open file '%s': %s", fullPath, PHYSFS_getLastError()));

    int fileSize = PHYSFS_fileLength(file);
    std::string buffer(fileSize, 0);
    PHYSFS_read(file, (void*)&buffer[0], 1, fileSize);
    PHYSFS_close(file);

    return buffer;
}

bool ResourceManager::writeFileBuffer(const std::string& fileName, const uchar* data, uint size)
{
    PHYSFS_file* file = PHYSFS_openWrite(fileName.c_str());
    if(!file) {
        g_logger.error(PHYSFS_getLastError());
        return false;
    }

    PHYSFS_write(file, (void*)data, size, 1);
    PHYSFS_close(file);
    return true;
}

bool ResourceManager::writeFileStream(const std::string& fileName, std::iostream& in)
{
    std::streampos oldPos = in.tellg();
    in.seekg(0, std::ios::end);
    std::streampos size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    in.read(&buffer[0], size);
    bool ret = writeFileBuffer(fileName, (const uchar*)&buffer[0], size);
    in.seekg(oldPos, std::ios::beg);
    return ret;
}

bool ResourceManager::writeFileContents(const std::string& fileName, const std::string& data)
{
    return writeFileBuffer(fileName, (const uchar*)data.c_str(), data.size());
}

FileStreamPtr ResourceManager::openFile(const std::string& fileName)
{
    std::string fullPath = resolvePath(fileName);

    PHYSFS_File* file = PHYSFS_openRead(fullPath.c_str());
    if(!file)
        stdext::throw_exception(stdext::format("unable to open file '%s': %s", fullPath, PHYSFS_getLastError()));
    return FileStreamPtr(new FileStream(fullPath, file, false));
}

FileStreamPtr ResourceManager::appendFile(const std::string& fileName)
{
    PHYSFS_File* file = PHYSFS_openAppend(fileName.c_str());
    if(!file)
        stdext::throw_exception(stdext::format("failed to append file '%s': %s", fileName, PHYSFS_getLastError()));
    return FileStreamPtr(new FileStream(fileName, file, true));
}

FileStreamPtr ResourceManager::createFile(const std::string& fileName)
{
    PHYSFS_File* file = PHYSFS_openWrite(fileName.c_str());
    if(!file)
        stdext::throw_exception(stdext::format("failed to create file '%s': %s", fileName, PHYSFS_getLastError()));
    return FileStreamPtr(new FileStream(fileName, file, true));
}

bool ResourceManager::deleteFile(const std::string& fileName)
{
    return PHYSFS_delete(resolvePath(fileName).c_str()) != 0;
}

bool ResourceManager::makeDir(const std::string directory)
{
    return PHYSFS_mkdir(directory.c_str());
}

std::list<std::string> ResourceManager::listDirectoryFiles(const std::string& directoryPath)
{
    std::list<std::string> files;
    auto rc = PHYSFS_enumerateFiles(resolvePath(directoryPath).c_str());

    for(int i = 0; rc[i] != NULL; i++)
        files.push_back(rc[i]);

    PHYSFS_freeList(rc);
    return files;
}

std::string ResourceManager::resolvePath(const std::string& path)
{
    std::string fullPath;
    if(stdext::starts_with(path, "/"))
        fullPath = path;
    else {
        std::string scriptPath = "/" + g_lua.getCurrentSourcePath();
        if(!scriptPath.empty())
            fullPath += scriptPath + "/";
        fullPath += path;
    }
    if(!(stdext::starts_with(fullPath, "/")))
        g_logger.traceWarning(stdext::format("the following file path is not fully resolved: %s", path));
    stdext::replace_all(fullPath, "//", "/");
    return fullPath;
}

std::string ResourceManager::getRealDir(const std::string& path)
{
    std::string dir;
    const char *cdir = PHYSFS_getRealDir(resolvePath(path).c_str());
    if(cdir)
        dir = cdir;
    return dir;
}

std::string ResourceManager::getCurrentDir()
{
    return g_platform.getCurrentDir();
}

std::string ResourceManager::getBaseDir()
{
    return PHYSFS_getBaseDir();
}

std::string ResourceManager::getUserDir()
{
    return PHYSFS_getUserDir();
}

std::string ResourceManager::guessFilePath(const std::string& filename, const std::string& type)
{
    if(isFileType(filename, type))
        return filename;
    return filename + "." + type;
}

bool ResourceManager::isFileType(const std::string& filename, const std::string& type)
{
    if(stdext::ends_with(filename, std::string(".") + type))
        return true;
    return false;
}
