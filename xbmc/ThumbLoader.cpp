/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "filesystem/StackDirectory.h"
#include "ThumbLoader.h"
#include "utils/URIUtils.h"
#include "URL.h"
#include "pictures/Picture.h"
#include "filesystem/File.h"
#include "FileItem.h"
#include "settings/GUISettings.h"
#include "GUIUserMessages.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/TextureManager.h"
#include "TextureCache.h"
#include "utils/log.h"
#include "programs/Shortcut.h"
#include "video/VideoInfoTag.h"
#include "settings/Settings.h" 
#include "settings/AdvancedSettings.h" 
#include "video/VideoDatabase.h"

#include "cores/dvdplayer/DVDFileInfo.h"

using namespace XFILE;
using namespace std;

CThumbLoader::CThumbLoader(int nThreads) :
  CBackgroundInfoLoader(nThreads)
{
}

CThumbLoader::~CThumbLoader()
{
}

bool CThumbLoader::LoadRemoteThumb(CFileItem *pItem)
{
  // look for remote thumbs
  CStdString thumb(pItem->GetThumbnailImage());
  if (!g_TextureManager.CanLoad(thumb))
  {
    CStdString cachedThumb(pItem->GetCachedVideoThumb());
    if (CFile::Exists(cachedThumb))
      pItem->SetThumbnailImage(cachedThumb);
    else
    {
      if (CPicture::CreateThumbnail(thumb, cachedThumb))
        pItem->SetThumbnailImage(cachedThumb);
      else
        pItem->SetThumbnailImage("");
    }
  }
  return pItem->HasThumbnail();
}

CStdString CThumbLoader::GetCachedThumb(const CFileItem &item)
{
  CTextureDatabase db;
  if (db.Open())
    return db.GetTextureForPath(item.m_strPath);
  return "";
}

bool CThumbLoader::CheckAndCacheThumb(CFileItem &item)
{
  if (item.HasThumbnail() && !g_TextureManager.CanLoad(item.GetThumbnailImage()))
  {
    CStdString thumb = CTextureCache::Get().CheckAndCacheImage(item.GetThumbnailImage());
    item.SetThumbnailImage(thumb);
    return !thumb.IsEmpty();
  }
  return false;
}

CThumbExtractor::CThumbExtractor(const CFileItem& item, const CStdString& listpath, bool thumb, const CStdString& target)
{
  m_listpath = listpath;
  m_target = target;
  m_thumb = thumb;
  m_item = item;

  m_path = item.m_strPath;

  if (item.IsVideoDb() && item.HasVideoInfoTag())
    m_path = item.GetVideoInfoTag()->m_strFileNameAndPath;

  if (URIUtils::IsStack(m_path))
    m_path = CStackDirectory::GetFirstStackedFile(m_path);
}

CThumbExtractor::~CThumbExtractor()
{
}

bool CThumbExtractor::operator==(const CJob* job) const
{
  if (strcmp(job->GetType(),GetType()) == 0)
  {
    const CThumbExtractor* jobExtract = dynamic_cast<const CThumbExtractor*>(job);
    if (jobExtract && jobExtract->m_listpath == m_listpath)
      return true;
  }
  return false;
}

bool CThumbExtractor::DoWork()
{
  if (URIUtils::IsLiveTV(m_path)
  ||  URIUtils::IsUPnP(m_path)
  ||  URIUtils::IsDAAP(m_path)
  ||  m_item.IsDVD()
  ||  m_item.IsDVDImage()
  ||  m_item.IsDVDFile(false, true)
  ||  m_item.IsInternetStream()
  ||  m_item.IsPlayList())
    return false;

  if (URIUtils::IsRemote(m_path) && !URIUtils::IsOnLAN(m_path))
    return false;

  bool result=false;
  if (m_thumb)
  {
    CLog::Log(LOGDEBUG,"%s - trying to extract thumb from video file %s", __FUNCTION__, m_path.c_str());
    result = CDVDFileInfo::ExtractThumb(m_path, m_target, &m_item.GetVideoInfoTag()->m_streamDetails);
    if(result)
    {
      m_item.SetProperty("HasAutoThumb", "1");
      m_item.SetProperty("AutoThumbImage", m_target);
      m_item.SetThumbnailImage(m_target);
    }
  }
  else if (m_item.HasVideoInfoTag() && !m_item.GetVideoInfoTag()->HasStreamDetails())
  {
    CLog::Log(LOGDEBUG,"%s - trying to extract filestream details from video file %s", __FUNCTION__, m_path.c_str());
    result = CDVDFileInfo::GetFileStreamDetails(&m_item);
  }

  return result;
}

CVideoThumbLoader::CVideoThumbLoader() :
  CThumbLoader(1), CJobQueue(true), m_pStreamDetailsObs(NULL)
{
}

CVideoThumbLoader::~CVideoThumbLoader()
{
  StopThread();
}

void CVideoThumbLoader::OnLoaderStart()
{
  m_AvailChecker.ResetCounter(); 
}

void CVideoThumbLoader::OnLoaderFinish()
{
  if (m_AvailChecker.JustOneSeason())
  {
    int iFlatten = g_guiSettings.GetInt("videolibrary.flattentvshows");
    if(g_settings.m_bMyVideoShowUnavailableMode)
    {
      if(iFlatten == 1)
      {
        for(int i = 0 ; i < this->m_pVecItems->Size() ; i++)
        {
          CFileItemPtr item = this->m_pVecItems->Get(i);
          
          if(item->GetVideoInfoTag()->m_iSeason > 0 && !item->GetPropertyBOOL("unavailable"))
          {
            // notify gui to refresh view
            // modified GUI_MSG_UPDATE's Params2 handling:
            // flags: +1 if want to use SetHistoryForPath
            //        +2 if want to cheat a little and change view without changing navigation history
            //           need to use it to trigger flatting tvshow when only one season is available
            //           and keep GUI thinking that is in Season Navigation
            CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0,0, GUI_MSG_UPDATE, 2);
            msg.SetStringParam(item->m_strPath);
            g_windowManager.SendThreadMessage(msg);
            m_AvailChecker.ResetCounter();
            return;
          }
        }
      }
      else
      {
        // if flatting tvshows is disabled and hiding unavailable items enabled
        for(int i = 0 ; i < this->m_pVecItems->Size() ; i++)
        {
          CFileItemPtr item = this->m_pVecItems->Get(i);
          
          if(item->GetVideoInfoTag()->m_iSeason == -1)
          {
            // 'removing' 'all episodes' from view when there is only 1 season available
            item->SetProperty("unavailable", true);
            break;
          }
        }
      }
    }
  }

  if (m_AvailChecker.AnyChanges())
  {
    CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_FILTER_ITEMS, 1);
    g_windowManager.SendThreadMessage(msg);
    m_AvailChecker.ResetCounter();
  }
}

/**
 * Look for a thumbnail for pItem.  If one does not exist, look for an autogenerated
 * thumbnail.  If that does not exist, attempt to autogenerate one.  Finally, check
 * for the existance of fanart and set properties accordinly.
 * @return: true if pItem has been modified
 */
bool CVideoThumbLoader::LoadItem(CFileItem* pItem)
{
  // real file availability checking is done here - first lets check if user enabled this feature
  if (g_guiSettings.GetBool("videolibrary.fileavailabilitychecking"))
  {
    m_AvailChecker.Check(pItem);
  }

  if (pItem->m_bIsShareOrDrive
  ||  pItem->IsParentFolder())
    return false;

  CFileItem item(*pItem);
  CStdString cachedThumb(item.GetCachedVideoThumb());

  if (!pItem->HasThumbnail())
  {
    item.SetUserVideoThumb();
    if (CFile::Exists(cachedThumb))
      pItem->SetThumbnailImage(cachedThumb);
    else
    {
      CStdString strPath, strFileName;
      URIUtils::Split(cachedThumb, strPath, strFileName);

      // create unique thumb for auto generated thumbs
      cachedThumb = strPath + "auto-" + strFileName;
      if (CFile::Exists(cachedThumb))
      {
        // this is abit of a hack to avoid loading zero sized images
        // which we know will fail. They will just display empty image
        // we should really have some way for the texture loader to
        // do fallbacks to default images for a failed image instead
        struct __stat64 st;
        if(CFile::Stat(cachedThumb, &st) == 0 && st.st_size > 0)
        {
          pItem->SetProperty("HasAutoThumb", "1");
          pItem->SetProperty("AutoThumbImage", cachedThumb);
          pItem->SetThumbnailImage(cachedThumb);
        }
      }
      else if (!item.m_bIsFolder && item.IsVideo() && g_guiSettings.GetBool("myvideos.extractthumb") &&
               g_guiSettings.GetBool("myvideos.extractflags"))
      {
        CThumbExtractor* extract = new CThumbExtractor(item, pItem->m_strPath, true, cachedThumb);
        AddJob(extract);
      }
    }
  }
  else if (!pItem->GetThumbnailImage().Left(10).Equals("special://"))
    LoadRemoteThumb(pItem);

  if (!pItem->HasProperty("fanart_image"))
  {
    if (pItem->CacheLocalFanart())
      pItem->SetProperty("fanart_image",pItem->GetCachedFanart());
  }

  if (!pItem->m_bIsFolder &&
       pItem->HasVideoInfoTag() &&
       g_guiSettings.GetBool("myvideos.extractflags") &&
       (!pItem->GetVideoInfoTag()->HasStreamDetails() ||
         pItem->GetVideoInfoTag()->m_streamDetails.GetVideoDuration() <= 0))
  {
    CThumbExtractor* extract = new CThumbExtractor(*pItem,pItem->m_strPath,false);
    AddJob(extract);
  }
  return true;
}

void CVideoThumbLoader::OnJobComplete(unsigned int jobID, bool success, CJob* job)
{
  if (success)
  {
    CThumbExtractor* loader = (CThumbExtractor*)job;
    loader->m_item.m_strPath = loader->m_listpath;
    CVideoInfoTag* info = loader->m_item.GetVideoInfoTag();
    if (m_pStreamDetailsObs)
      m_pStreamDetailsObs->OnStreamDetails(info->m_streamDetails, info->m_strFileNameAndPath, info->m_iFileId);
    if (m_pObserver)
      m_pObserver->OnItemLoaded(&loader->m_item);
    CFileItemPtr pItem(new CFileItem(loader->m_item));
    CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_ITEM, 0, pItem);
    g_windowManager.SendThreadMessage(msg);
  }
  CJobQueue::OnJobComplete(jobID, success, job);
}

CProgramThumbLoader::CProgramThumbLoader()
{
}

CProgramThumbLoader::~CProgramThumbLoader()
{
}

bool CProgramThumbLoader::LoadItem(CFileItem *pItem)
{
  if (pItem->IsParentFolder()) return true;
  return FillThumb(*pItem);
}

bool CProgramThumbLoader::FillThumb(CFileItem &item)
{
  // no need to do anything if we already have a thumb set
  if (CheckAndCacheThumb(item) || item.HasThumbnail())
    return true;

  // see whether we have a cached image for this item
  CStdString thumb = GetCachedThumb(item);
  if (!thumb.IsEmpty())
  {
    item.SetThumbnailImage(CTextureCache::Get().CheckAndCacheImage(thumb));
    return true;
  }
  thumb = GetLocalThumb(item);
  if (!thumb.IsEmpty())
  {
    CTextureDatabase db;
    if (db.Open())
      db.SetTextureForPath(item.m_strPath, thumb);
    thumb = CTextureCache::Get().CheckAndCacheImage(thumb);
  }
  item.SetThumbnailImage(thumb);
  return true;
}

CStdString CProgramThumbLoader::GetLocalThumb(const CFileItem &item)
{
  // look for the thumb
  if (item.IsShortCut())
  {
    CShortcut shortcut;
    if ( shortcut.Create( item.m_strPath ) )
    {
      // use the shortcut's thumb
      if (!shortcut.m_strThumb.IsEmpty())
        return shortcut.m_strThumb;
      else
      {
        CFileItem cut(shortcut.m_strPath,false);
        if (FillThumb(cut))
          return cut.GetThumbnailImage();
      }
    }
  }
  else if (item.m_bIsFolder)
  {
    CStdString folderThumb = item.GetFolderThumb();
    if (XFILE::CFile::Exists(folderThumb))
      return folderThumb;
  }
  else
  {
    CStdString fileThumb(item.GetTBNFile());
    if (CFile::Exists(fileThumb))
      return fileThumb;
  }
  return "";
}

CMusicThumbLoader::CMusicThumbLoader()
{
}

CMusicThumbLoader::~CMusicThumbLoader()
{
}

bool CMusicThumbLoader::LoadItem(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive) return true;
  if (!pItem->HasThumbnail())
    pItem->SetUserMusicThumb();
  else
    LoadRemoteThumb(pItem);
  return true;
}

void CAvailabilityChecker::CompareUnavailibility(bool new_unavailability, CFileItem *pItem)
{
  if (new_unavailability != pItem->GetPropertyBOOL("unavailable"))
  {
    pItem->SetProperty("unavailable", new_unavailability);
    this->m_bAnyChanges = true;
  }
}

bool CAvailabilityChecker::AnyChanges()
{
  return this->m_bAnyChanges;
}

void CAvailabilityChecker::ResetCounter()
{
  this->m_bAnyChanges = false;
}

void CVideoAvailabilityChecker::ResetCounter()
{
  this->m_bAnyChanges = false;
  this->m_iTvShowSeasonsAvail = 0;
  this->m_bTvShowSeasons = false;
}

void CVideoAvailabilityChecker::Check(CFileItem *pItem)
{
  if (pItem->m_bIsFolder)
  {
    // just tvshows now
    CVideoInfoTag *tag = pItem->GetVideoInfoTag();
    if (tag->m_iEpisode > 0)
    {
      //if episode count > 0 then we identify item as tvshow or tvshow season
      CVideoDatabase dbs;
      dbs.Open();
      std::vector<CStdString> episodelist;
      if (tag->m_iSeason > -1)
      {
        m_bTvShowSeasons = true;
        //if season number > -1 we identify item as tvshow season
        if (g_advancedSettings.m_bVideoLibraryAvailabilityCheckingByFile)
          dbs.GetFileNamesForSeason(tag->m_iDbId, tag->m_iSeason, episodelist);
        else
          dbs.GetPathsForSeason(tag->m_iDbId, tag->m_iSeason, episodelist);
      }
      else
      {
        //if season number == -1 we identify item as tvshow
        if (g_advancedSettings.m_bVideoLibraryAvailabilityCheckingByFile)
          dbs.GetFileNamesForTvShow(tag->m_iDbId, episodelist);
        else
          dbs.GetPathsForTvShow(tag->m_iDbId, episodelist);
      }
      dbs.Close();

      bool unavailable = true;
      for (unsigned int i = 0 ; i < episodelist.size();i++)
      {
        CFileItem fs_item(episodelist[i], !g_advancedSettings.m_bVideoLibraryAvailabilityCheckingByFile);
        if (fs_item.Exists(false))
        {
          unavailable = false;
          break;
        }
      }
      CompareUnavailibility(unavailable, pItem);

      if(this->m_bTvShowSeasons && !unavailable)
        this->m_iTvShowSeasonsAvail++;
    } // closing bracket if(tag->m_iEpisode > 0)
  } // closing bracket if(pItem->m_bIsFolder)
  else
  {
    //it's eiter tvshow's episode or movie
    CompareUnavailibility(!pItem->Exists(false), pItem);
  }
}

bool CVideoAvailabilityChecker::JustOneSeason()
{
  return ( this->m_bTvShowSeasons && this->m_iTvShowSeasonsAvail < 2 );
}

