/*
* This file is part of Wakanda software, licensed by 4D under
*  (i) the GNU General Public License version 3 (GNU GPL v3), or
*  (ii) the Affero General Public License version 3 (AGPL v3) or
*  (iii) a commercial license.
* This file remains the exclusive property of 4D and/or its licensors
* and is protected by national and international legislations.
* In any event, Licensee's compliance with the terms and conditions
* of the applicable license constitutes a prerequisite to any use of this file.
* Except as otherwise expressly stated in the applicable license,
* such license does not include any other license or rights on this file,
* 4D's and/or its licensors' trademarks and/or other proprietary rights.
* Consequently, no title, copyright or other proprietary rights
* other than those specified in the applicable license is granted.
*/
#include "headers4d.h"
#include "VSolution.h"
#include "VProjectItem.h"
#include "VProjectItemBehaviour.h"
#include "VRIASettingsKeys.h"
#include "VRIASettingsFile.h"
#include "VProjectSettings.h"
#include "VRIAServerConstants.h"
#include "VRIAUTIs.h"
#include "VSourceControl.h"
#include "DB4D/Headers/DB4D.h"
#include "VProject.h"

#if !RIA_SERVER
#include "VSourceControlConnector.h"
#endif

USING_TOOLBOX_NAMESPACE



// ----------------------------------------------------------------------------



class VProjectFileStampSaver : public VObject
{
public:
	VProjectFileStampSaver( VProject *inProject) : fProject(inProject)
	{
		fDirtyStampSave = fProject->_GetProjectFileDirtyStamp();
	}

	virtual ~VProjectFileStampSaver() {;}

			bool StampHasBeenChanged() const
			{
				return fProject->_GetProjectFileDirtyStamp() > fDirtyStampSave;
			}

private:
			VProject	*fProject;
			sLONG		fDirtyStampSave;
};



// ----------------------------------------------------------------------------
// Classe VProject :
// ----------------------------------------------------------------------------
VProject::VProject( VSolution *inParentSolution)
: fSolution(inParentSolution)
,fProjectItem( NULL)
,fProjectItemProjectFile( NULL)
,fSourceControlConnector(NULL)
,fProjectFileDirtyStamp(0)
,fIsLoadingFromXML(false)
,fSymbolTable(NULL)
,fBackgroundDeleteFileTask(NULL)
,fItemParsingCompleteSignal(NULL)
,fRPCFilesParsingCompleteEvent(NULL)
,fProjectUser(NULL)
,fIsWatchingFileSystem(false)
,fIsUpdatingSymbolTable(false)
{
	fUUID.Regenerate();

#if !RIA_SERVER
	fSourceControlConnector = new VSourceControlConnector(this);
#endif

	fProjectUser = new VProjectUser(this);
}

VProject::~VProject()
{
	xbox_assert(!fIsWatchingFileSystem);
	xbox_assert(!fIsUpdatingSymbolTable);
	xbox_assert(fBackgroundDeleteFileTask == NULL);
	xbox_assert(fSymbolTable == NULL);

#if !RIA_SERVER
#if 0	// to be fixed: crashing bug
	delete fSourceControlConnector;
	fSourceControlConnector = NULL;
#endif
#endif

	ReleaseRefCountable( &fItemParsingCompleteSignal);

	delete fProjectUser;
	fProjectUser = NULL;

	if (fRPCFilesParsingCompleteEvent != NULL)
	{
		if (fRPCFilesParsingCompleteEvent->Unlock())
			ReleaseRefCountable( &fRPCFilesParsingCompleteEvent);
	}

	ReleaseRefCountable( &fItemParsingCompleteSignal);
}


VProject* VProject::Instantiate( XBOX::VError& outError, VSolution *inParentSolution, const XBOX::VFilePath& inProjectFile)
{
	VFilePath folderPath;
	if (!inProjectFile.IsFile() || !inProjectFile.GetFolder( folderPath))
	{
		outError = VE_INVALID_PARAMETER;
		return NULL;
	}

	VProjectItem *rootItem = NULL, *projectFileItem = NULL;
	outError = VE_OK;

	// Create the root project item
	rootItem = new VProjectItem( VURL( folderPath), VProjectItem::ePROJECT);
	if (rootItem != NULL)
	{
		VString itemName;
		inProjectFile.GetFileName( itemName, false);
		rootItem->SetName( itemName);
		rootItem->SetDisplayName( itemName);

		// Create an item for the project file
		projectFileItem = new VProjectItem( VURL( inProjectFile), VProjectItem::eFILE);
		if (projectFileItem != NULL)
		{
			inProjectFile.GetFileName( itemName);
			projectFileItem->SetName( itemName);
			projectFileItem->SetDisplayName( itemName);
		}
		else
		{
			outError = VE_MEMORY_FULL;
		}
	}
	else
	{
		outError = VE_MEMORY_FULL;
	}
	
	VProject *project = NULL;

	if (outError == VE_OK)
	{
		project = new VProject( inParentSolution);
		if (project != NULL)
		{
			project->_SetProjectItem( rootItem);
			project->_SetProjectFileItem( projectFileItem);
		}
		else
		{
			outError = VE_MEMORY_FULL;
		}
	}
	
	ReleaseRefCountable( &rootItem);
	ReleaseRefCountable( &projectFileItem);

	return project;
}


const VUUID& VProject::GetUUID() const
{
	return fUUID;
}


bool VProject::GetName( XBOX::VString& outName) const
{
	bool ok = false;
	outName.Clear();

	if (fProjectItem != NULL)
	{
		outName = fProjectItem->GetDisplayName();
		ok = true;
	}
	return ok;
}


bool VProject::GetProjectFolderPath( XBOX::VFilePath& outPath) const
{
	bool ok = false;
	outPath.Clear();

	if (fProjectItem != NULL)
		ok = fProjectItem->GetFilePath( outPath);

	return ok;
}


bool VProject::GetProjectFilePath( XBOX::VFilePath& outPath) const
{
	bool ok = false;
	outPath.Clear();

	if (fProjectItemProjectFile != NULL)
		ok = fProjectItemProjectFile->GetFilePath( outPath);

	return ok;
}


XBOX::VError VProject::Rename( const XBOX::VString& inNewName)
{
	VError err = VE_OK;

	VString name;
	if (GetName( name) && (name != inNewName))
	{
		UnregisterProjectItemFromMapOfFullPathes( fProjectItemProjectFile);

		name = inNewName + L"." + RIAFileKind::kProjectFileExtension;

		err = fProjectItemProjectFile->RenameContent( name);
		if (err == VE_OK)
		{
			fProjectItem->SetDisplayName( inNewName);
			fProjectItem->Touch();
		}

		RegisterProjectItemInMapOfFullPathes( fProjectItemProjectFile);
	}


	if (err != VE_OK)
		err = VE_SOMA_CANNOT_RENAME_PROJECT;

	return err;
}


void VProject::BuildPathFromRelativePath( XBOX::VFilePath& outFilePath, const XBOX::VString& inRelativePath, XBOX::FilePathStyle inRelativePathStyle) const
{
	outFilePath.Clear();

	if (fProjectItem != NULL)
	{
		VFilePath folderPath;
		fProjectItem->GetFilePath( folderPath);
		folderPath = folderPath.ToFolder();
		outFilePath.FromRelativePath(folderPath, inRelativePath, inRelativePathStyle);
	}	
}


sLONG VProject::BackgroundDeleteFile( VTask *inTask )
{
	// We use a handshake, wink and a nod between the call to remove a project item's
	// file, and the task which actually performs the operation.
	struct temp_struct { ISymbolTable *s;  VString *p; };
	temp_struct *ts = (temp_struct *)inTask->GetKindData();
	VString *itemPath = ts->p;
	ISymbolTable *inSymTable = ts->s->CloneForNewThread();

	// Given the project item, locate its IFile record
	std::vector< Symbols::IFile * > files = inSymTable->GetFilesByPathAndBaseFolder( VFilePath(*itemPath), eSymbolFileBaseFolderProject );
	if (files.size() == 1) {
		inSymTable->DeleteFile( files.front() );
	}
	
	inSymTable->Release();

	return 0;
}


void VProject::LoadCoreFiles( const VFolder &inFolder, const VString &inName, ESymbolFileExecContext inExecContext, IDocumentParserManager::IJob *inJob )
{
	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager)
		return;

	// First, we want to load up the order.txt file -- this will tell us the order we want
	// to load the JavaScript files.
	VFolder coreFolder( inFolder, inName );
	VFile order( coreFolder, CVSTR( "order.txt" ) );
	if (order.Exists() )
	{
		VFileStream stream( &order );
		if (VE_OK == stream.OpenReading()) {
			// Each line in the file tells us the name of what to parse next
			VString fileName;
			while (VE_STREAM_EOF != stream.GetTextLine( fileName, false ))
			{
				VSymbolFileInfos fileInfos(VFile( coreFolder, fileName ).GetPath(), eSymbolFileBaseFolderStudio, inExecContext);

				inJob->ScheduleTask(fileInfos);
			}
			stream.CloseReading();
		}
	}
}

void VProject::LoadDefaultSymbols( ISymbolTable *table )
{
	VFolder *folder = VProcess::Get()->RetainFolder( VProcess::eFS_Resources);
	if (folder == NULL)
		return;

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager)
		return;

	VFilePath path;
	folder->GetPath( path );

	path.ToSubFolder( "JavaScript Support Files" );
	VFolder supportFolder( path );

	IDocumentParserManager::IJob *job = parsingManager->CreateJob();

	LoadCoreFiles( supportFolder, CVSTR( "bothSides" ), eSymbolFileExecContextClientServer, job );
	LoadCoreFiles( supportFolder, CVSTR( "serverSide" ), eSymbolFileExecContextServer, job );
	LoadCoreFiles( supportFolder, CVSTR( "clientSide" ), eSymbolFileExecContextClient, job );
	
	// Parsing "waf.js" in Studio resources "walib" folder
	VFilePath	wafPath;
	VString		wafPathStr;

	folder->GetPath( wafPath);
	wafPath = wafPath.ToSubFolder( L"Web Components");
	wafPath = wafPath.ToSubFolder( L"walib");
	wafPath = wafPath.ToSubFolder( L"WAF");
	wafPath = wafPath.ToSubFile( L"waf.js");

	// Tmp : desactivate "waf.js" parsing while memory leaks still remain
	// job->ScheduleTask( VSymbolFileInfos (wafPath, eSymbolFileBaseFolderStudio, eSymbolFileExecContextClient) );

	folder->Release();

	// Now that we've collected all of the default symbols into one batch, we can submit it as an entire job.
	parsingManager->ScheduleTask( this, job, IDocumentParserManager::CreateCookie( 'Core', this), table );
	job->Release();
}

void VProject::LoadKludgeSymbols( ISymbolTable *table )
{
	VFolder *folder = VProcess::Get()->RetainFolder( VProcess::eFS_Resources);
	if (folder == NULL)
		return;

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager)
		return;

	VFilePath path;
	folder->GetPath( path );

	path.ToSubFolder( "JavaScript Support Files" );
	VFolder supportFolder( path );

	IDocumentParserManager::IJob *job = parsingManager->CreateJob();

	LoadKludgeFile( supportFolder, CVSTR( "bothSides" ), eSymbolFileExecContextClientServer, job );
	LoadKludgeFile( supportFolder, CVSTR( "serverSide" ), eSymbolFileExecContextServer, job );
	LoadKludgeFile( supportFolder, CVSTR( "clientSide" ), eSymbolFileExecContextClient, job );
	
	folder->Release();
}

void VProject::LoadKludgeFile( const VFolder &inFolder, const VString &inName, ESymbolFileExecContext inExecContext, IDocumentParserManager::IJob *inJob )
{
	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if ( NULL != parsingManager )
	{
		VFolder	coreFolder( inFolder, inName );
		VFile	kludgeFile( coreFolder, CVSTR("kludge.js") );
		
			if ( kludgeFile.Exists() )
			{
				VSymbolFileInfos fileInfos(kludgeFile.GetPath(), eSymbolFileBaseFolderStudio, inExecContext);
				parsingManager->ScheduleTask( this, fileInfos, IDocumentParserManager::CreateCookie( 'Klu', this), fSymbolTable, IDocumentParserManager::kPriorityAboveNormal );
			}
	}
}

void VProject::ParseProjectItemForEditor( VProjectItem *inItem, VString& inContents)
{
	// We've been asked to schedule this item for parsing explicitly for an editor, so we want to 
	// schedule a parsing task for it, but we want to do so at a higher priority level.  This ensures
	// that the editor gets more up to date information quicker, especially if the file is already in the
	// parsing queue.

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager)
		return;	

	parsingManager->GetParsingCompleteSignal().Connect( this, VTask::GetCurrent(), &VProject::ParsingComplete );

	if (inItem && fSymbolTable) {
		VFilePath path;
		if (inItem->GetFilePath( path ))
		{
			VProjectItem*			webFolderProjectItem = GetProjectItemFromTag(kWebFolderTag);
			ESymbolFileExecContext	execContext = ((inItem == webFolderProjectItem) || inItem->IsChildOf(webFolderProjectItem)) ? eSymbolFileExecContextClient : eSymbolFileExecContextServer;
			VSymbolFileInfos		fileInfos(path, eSymbolFileBaseFolderProject, execContext);			
			
			parsingManager->ScheduleTask( this, fileInfos, inContents, IDocumentParserManager::CreateCookie( 'Proj', this), fSymbolTable, IDocumentParserManager::kPriorityAboveNormal );
		}
	}
}

VProject::ItemParsingCompleteSignal* VProject::RetainItemParsingCompleteSignal() const
{
	return RetainRefCountable( fItemParsingCompleteSignal);
}

void VProject::FileSystemEventHandler( const std::vector< VFilePath > &inFilePaths, VFileSystemNotifier::EventKind inKind )
{
#if SOLUTION_PROFILING_ENABLED
	VString solutionName, profilName( L"VProject::FileSystemEventHandler ");
	VFilePath path;
	fSolution->GetSolutionFolderPath( path);
	path.ToParent();
	fSolution->GetName( solutionName);
	profilName += L" items count: ";
	profilName.AppendLong( inFilePaths.size());
	profilName += L", event: ";

	if ((inKind & VFileSystemNotifier::kFileModified) != 0)
		profilName += L"modifying=true, ";
	else
		profilName += L"modifying=false, ";

	if ((inKind & VFileSystemNotifier::kFileDeleted) != 0)
		profilName += L"deleting=true, ";
	else
		profilName += L"deleting=false, ";

	if ((inKind & VFileSystemNotifier::kFileAdded) != 0)
		profilName += L"adding=true, ";
	else
		profilName += L"adding=false, ";

	VSolutionProfiler::Get()->StartProfiling( profilName, path, solutionName);
#endif

	// We've been notified that something has happened within a directory we're watching.  We will
	// look to see whether any of the files are ones we want to parse.  For right now, we will only
	// check JavaScript files -- and we also want to test to see whether one of the files is currently
	// being edited, and if it is, bump the priority up.
	if (inKind == VFileSystemNotifier::kFileModified) 
	{
		IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
		if (parsingManager) 
		{
			// We need the webfolder item to know if new added files will be executed in
			// client or server context
			VProjectItem*	webFolderProjectItem = GetProjectItemFromTag(kWebFolderTag);

			for (std::vector< VFilePath >::const_iterator iter = inFilePaths.begin(); iter != inFilePaths.end(); ++iter) 
			{
				VProjectItem *projectItem = GetProjectItemFromFullPath( iter->GetPath() );
				if (projectItem) 
				{
					if (projectItem->ConformsTo( RIAFileKind::kCatalogFileKind))
					{
						if (GetSolution()->AcceptReloadCatalog())
						{
							// temporary fix to avoid reentrance when ReloadCatalog modifies the catalog file
							GetSolution()->SetAcceptReloadCatalog(false);
							GetSolution()->ReloadCatalog(projectItem);
							GetSolution()->SetAcceptReloadCatalog(true);
						}
					}
	
					if (iter->MatchExtension( RIAFileKind::kJSFileExtension, false ) || (projectItem->ConformsTo( RIAFileKind::kCatalogFileKind)) ) 
					{
						IDocumentParserManager::Priority priority = IDocumentParserManager::kPriorityNormal;
						if (fSolution->GetDelegate() != NULL)
							priority = fSolution->GetDelegate()->GetDocumentParsingPriority( *iter);

						if (projectItem->ConformsTo( RIAFileKind::kCatalogFileKind))
						{
							IEntityModelCatalog *catalog = projectItem->GetBehaviour()->GetCatalog();
							if (NULL != catalog)
								parsingManager->ScheduleTask( this, catalog, IDocumentParserManager::CreateCookie( 'Proj', this), fSymbolTable, priority );
						}
						else
						{
							ESymbolFileExecContext	execContext = ((projectItem == webFolderProjectItem) || projectItem->IsChildOf(webFolderProjectItem)) ? eSymbolFileExecContextClient : eSymbolFileExecContextServer;
							VSymbolFileInfos		fileInfos(*iter, eSymbolFileBaseFolderProject, execContext);

							parsingManager->ScheduleTask( this, fileInfos, IDocumentParserManager::CreateCookie( 'Proj', this), fSymbolTable, priority );
						}
					}
				}
			}
		}

		if (fSolution->GetDelegate() != NULL)
			fSolution->GetDelegate()->SynchronizeFromSolution();

		if (!inFilePaths.empty())
			GetSolution()->DoProjectItemsChanged( inFilePaths);
	} 
	else 
	{
		if (inKind == VFileSystemNotifier::kFileDeleted)
			fDeletedFilePaths.insert( fDeletedFilePaths.end(), inFilePaths.begin(), inFilePaths.end());

		// A file was added, renamed, or deleted.  So we want to resynchronize the containing folder with the file
		// system.
		SynchronizeWithFileSystem( GetProjectItem());

		if (fSolution->GetDelegate() != NULL)
			fSolution->GetDelegate()->SynchronizeFromSolution();
	}

#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->EndProfiling();
#endif
}

void VProject::ParsingComplete( VFilePath inPath, sLONG inStatus, IDocumentParserManager::TaskCookie inCookie )
{
	if (inCookie.fCookie != this)
		return;
	
	if (inCookie.fIdentifier == 'Proj')
	{
		if (inStatus == 0)
		{
			VProjectItem *projItem = GetProjectItemFromFullPath( inPath.GetPath() );
			if (projItem)
			{
				if (fSolution->GetDelegate() != NULL)
					fSolution->GetDelegate()->ParsingComplete( projItem);

				if (fItemParsingCompleteSignal != NULL)
				{
					fItemParsingCompleteSignal->Trigger( this, projItem);
				}
			}
		}
	}
	else if (inCookie.fIdentifier == 'Core')
	{
		GetSolution()->DoParsedOneFile();
	}
}

void VProject::BackgroundParseFiles()
{
	// If we don't have a symbol table, then we want to try to generate a new
	// one.  If that fails, we need to bail out.
	// sc 16/06/2010, do nothing if the symbol table is not opened
	if (fSymbolTable == NULL)
		return;

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager) 
		return;

#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Start updating symbol table of project " + lName + L"\n");
#endif

#if RIA_STUDIO

	// Always load the default symbols
	LoadDefaultSymbols( fSymbolTable );

	VectorOfProjectItems items;
	_GetProjectItemsByExtension( RIAFileKind::kJSFileExtension, items );

	// Some people have projects with multiple versions of the same file -- they have
	// different information in the files, and the files have different names, but they're
	// actually the "same" file.  For instance: foo.js, foo-debug.js and foo-min.js.  The 
	// -debug file is one with debugging information in it, and -min is one that's been run
	// through a minimizer.  If the user has files like this, we only want to parse *one* of
	// the files, or else we'll end up with a bunch of extra symbols that we don't actually 
	// want.  However, this leaves us with another issue, which is one of ordering.  We don't 
	// know the order that the files will be given to us from the call to GetProjectItemsByExtension!
	// So we have to take that list, see if we can find any "duplicate" files within it, and then
	// decide which file to keep.  It's a lot more convoluted than it should be, but that seems
	// to be the norm for JavaScript anyways.  Oh, and there's another lovely problem -- we can't
	// just skip any files with a -debug or -min in them because it's also possible that the user 
	// doesn't have multiple versions of that file.  For instance, they only dragged the -debug 
	// version into their project!  So we are going to map hashes to project items.  We will hash
	// the first "part" of the file name (where a part is defined by the dash).  Then, if we find
	// multiple files, we can determine whether the file we've mapped or the conflicting item is a
	// better choice as to what to parse.  So, for instance, say we find foo-min.js first, and then
	// we get a conflict with foo-debug.js.  The debug file is a better choice to parse since it
	// will contain more symbol information.  So we will replace the foo-min.js file with the foo-debug.js
	// file, and continue searching.  According to Alexandre, the preference for files is: debug (1), 
	// regular (2), min (3), but that may change in the future.  So when we process an item, we will also
	// map it's name hash to a weight.  This will help us to determine whether to replace the file or not.
	// The higher the weight, the harder it is to replace the file.
	std::map< sLONG, VProjectItem * > itemList;
	std::map< sLONG, int > weightList;

	for (VectorOfProjectItems::iterator iter = items.begin(); iter != items.end(); ++iter) {
		// Ghosted items are ones that have been removed from the project already
		if ((*iter)->IsGhost())	continue;

		// Get the project item's file name
		VString pathStr;
		(*iter)->BuildFullPath( pathStr );

		// Get just the file name
		VFilePath path( pathStr );
		VString fileName;
		path.GetFileName( fileName, false );

		// Now that we have the file name, we want to split it up into parts based on the dash (if there
		// is one in the file name at all).
		VectorOfVString parts;
		fileName.GetSubStrings( (UniChar)'-', parts, true, true );	// we want empty strings and to trim spaces

		// We want to strip off the last part of the name if it's -debug or -min.  We can't just rely on the
		// front part as being correct because the user might have a file named foo-bar-debug.js and foo-baz-min.js,
		// in which cases we'd only hash foo and never add symbols for foo-baz-min.js even though it's a distinct
		// file.
		VString lastPart;
		if (parts.size() > 1) {
			if (parts.back() == "debug" || parts.back() == "min") {
				// We want to trim this part off
				lastPart = parts.back();
				parts.pop_back();
			}

			// Now create the new file name
			fileName = "";
			for (int i = 0; i < parts.size(); i++) {
				fileName += parts[ i ];
				if (i != parts.size() - 1) fileName += CVSTR( "-" );
			}
		} else {
			fileName = parts.front();
		}

		// The first part is the file name that we actually care about, so hash that
		uLONG hash = fileName.GetHashValue();

		// The last part of the file is the weight.  If the last part is -min, the weight is 1, if the last
		// part doesn't exist (because there aren't parts!), the weight is 2, if the last part is -debug, the
		// weight is 3.
		enum { kNoPriority, kLowPriority, kMediumPriority, kHighPriority };
		int weight = kNoPriority;
		if (lastPart == "min")			weight = kLowPriority;
		else if (lastPart == "debug" )	weight = kHighPriority;
		else							weight = kMediumPriority;
		
		// Now we want to see if there's already an item in the map with the same hash.  If not, then we
		// can just add this project item.  But if there is, we need to see whether we want to replace or not.
		std::map< sLONG, VProjectItem * >::iterator listVal = itemList.find( hash );
		if (listVal == itemList.end()) {
			// This is the easy case, we can just add the item to the list
			itemList[ hash ] = *iter;

			// Also, map the weight of the file
			weightList[ hash ] = weight;
		} else {
			// An item already exists in the list, so let's see whether we want to replace it or not.  We will determine
			// that based on the weight of the hash.
			int existingFileWeight = weightList[ listVal->first ];
			if (weight > existingFileWeight) {
				// The weight of the current file beats the weight of the file we had, so replace
				itemList[ hash ] = *iter;
				weightList[ hash ] = weight;
			}
		}
	}

	VProjectItem* webFolderProjectItem = GetProjectItemFromTag(kWebFolderTag);

	// Now that we finally have the item list figured out, we can actually generate the symbols for it.
	IDocumentParserManager::IJob *job = parsingManager->CreateJob();
	for (std::map< sLONG, VProjectItem * >::iterator iter = itemList.begin(); iter != itemList.end(); ++iter) {

		bool	belongToWebFolder;
		VString pathStr;		

		belongToWebFolder = ((iter->second == webFolderProjectItem) || iter->second->IsChildOf(webFolderProjectItem));
		iter->second->BuildFullPath( pathStr );

		VFilePath			path( pathStr );		
		VSymbolFileInfos	fileInfos(path, eSymbolFileBaseFolderProject, belongToWebFolder ? eSymbolFileExecContextClient : eSymbolFileExecContextServer);

		job->ScheduleTask(fileInfos);
	}

	// Now we can schedule the batch job
	GetSolution()->DoStartParsingFiles( job->GetTaskCount());

	parsingManager->GetJobCompleteSignal().Connect( this, VTask::GetCurrent(), &VProject::CoreFileLoadComplete );
	parsingManager->ScheduleTask( this, job, IDocumentParserManager::CreateCookie( 'Proj', this), fSymbolTable );
	job->Release();

#elif RIA_SERVER

	VectorOfProjectItems itemsVector;
	
	GetProjectItemsFromTag( kRPCMethodTag, itemsVector);
	if (!itemsVector.empty())
	{
		IDocumentParserManager::IJob *job = parsingManager->CreateJob();
		if (job != NULL)
		{
			VFilePath path;
			for (VectorOfProjectItemsIterator iter = itemsVector.begin() ; iter != itemsVector.end() ; ++iter)
			{
				(*iter)->GetFilePath( path);
				job->ScheduleTask( VSymbolFileInfos	(path, eSymbolFileBaseFolderProject, eSymbolFileExecContextServer) );
			}

			if (fRPCFilesParsingCompleteEventMutex.Lock())
			{
				if (fRPCFilesParsingCompleteEvent == NULL)
					fRPCFilesParsingCompleteEvent = new VSyncEvent();

				fRPCFilesParsingCompleteEventMutex.Unlock();
			}

			parsingManager->GetJobCompleteSignal().Connect( this, VTask::GetCurrent(), &VProject::RPCFilesParsingComplete);
			parsingManager->ScheduleTask( this, job, IDocumentParserManager::CreateCookie( 'RPPj', this), fSymbolTable);
			ReleaseRefCountable( &job);
		}
	}

#endif

	fIsUpdatingSymbolTable = true;
}

void VProject::CoreFileLoadComplete( IDocumentParserManager::TaskCookie inCookie )
{
	if (inCookie.fCookie != this)
		return;

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager) 
		return;

	// Ideally, loading entity models would be part of the job itself.  However, that's not easily accomplished
	// because we can't access the VCatalog outside of the main project (it's not an interface that we can use
	// from within the language syntax component).  So we'll just parse all of the entity model stuff once the 
	// job has completed.  The only tricky part is determining whether we need to actually process the catalog
	// file -- it may not have changed, in which case we don't need to regenerate the symbols for it.  Given 
	// the speed at which the symbols are generated, we'll just do it every time.
	if (inCookie.fIdentifier == 'Core') {
		VProjectItem *catalogItem = _GetEntityModelProjectItem(); 
		if (catalogItem) {
			IEntityModelCatalog *catalog = catalogItem->GetBehaviour()->GetCatalog();
			if (catalog) {
				parsingManager->ScheduleTask( this, catalog, IDocumentParserManager::CreateCookie( 'Proj', this), fSymbolTable );
			}
		}

		LoadKludgeSymbols( fSymbolTable );

		GetSolution()->DoStopParsingFiles();
	}
}


void VProject::RPCFilesParsingComplete( IDocumentParserManager::TaskCookie inCookie)
{
	if (inCookie.fCookie != this)
		return;

	if (inCookie.fIdentifier == 'RPPj')
	{
		if (fRPCFilesParsingCompleteEventMutex.Lock())
		{
			if (fRPCFilesParsingCompleteEvent != NULL)
			{
				if (fRPCFilesParsingCompleteEvent->Unlock())
					ReleaseRefCountable( &fRPCFilesParsingCompleteEvent);
			}
			fRPCFilesParsingCompleteEventMutex.Unlock();
		}
	}
}


void VProject::StopBackgroundDeleteFiles()
{
	if (fBackgroundDeleteFileTask) {
		if (fBackgroundDeleteFileTask->GetState() >= TS_RUNNING) {
			fBackgroundDeleteFileTask->Kill();
			fBackgroundDeleteFileTask->StopMessaging();
		}
		fBackgroundDeleteFileTask->Release();
		fBackgroundDeleteFileTask = NULL;
	}
}


void VProject::StopBackgroundParseFiles()
{
#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Stop updating symbol table of project " + lName + L"\n");
#endif

	IDocumentParserManager *parsingManager = GetSolution()->GetDocumentParserManager();
	if (!parsingManager) 
		return;

	parsingManager->GetJobCompleteSignal().Disconnect( this);	// sc 15/04/2010, the project was never disconnected from this signal
	parsingManager->GetParsingCompleteSignal().Disconnect( this );
	parsingManager->UnscheduleTasksForHandler( this );

	fIsUpdatingSymbolTable = false;
}


VSyncEvent* VProject::RetainRPCFilesParsingCompleteEvent() const
{
	VSyncEvent *syncEvent = NULL;

	if (fRPCFilesParsingCompleteEventMutex.Lock())
	{
		syncEvent = RetainRefCountable( fRPCFilesParsingCompleteEvent);

		fRPCFilesParsingCompleteEventMutex.Unlock();
	}
	
	return syncEvent;
}


void VProject::RemoveProjectItemFromSymbolTable(VProjectItem *inProjectItem)
{
	if (!fSymbolTable)	return;
	if (!inProjectItem)	return;

	// We use a handshake, wink and a nod between the call to remove a project item's
	// file, and the task which actually performs the operation.
	struct temp_struct { ISymbolTable *s;  VString *p; };
	static temp_struct ts;	// We make it static so that it persists
	static VString itemFullPath;

	if ( inProjectItem->BuildFullPath(itemFullPath) )
	{
		ts.s = fSymbolTable;
		ts.p = &itemFullPath;

		// I don't believe this is quite the correct operation.  What we really want to
		// do is queue up another file to be removed instead of bailing out partially
		// through the existing file being removed.  Doing this will leave the database
		// in a state where the file's symbols haven't been entirely removed.
		StopBackgroundDeleteFiles();

		// Start a task that background parses all of the files in the given project
		fBackgroundDeleteFileTask = new VTask( this, 0, eTaskStylePreemptive, &BackgroundDeleteFile );
		fBackgroundDeleteFileTask->SetKindData( (sLONG_PTR)&ts );
		fBackgroundDeleteFileTask->Run();
	}
}

bool _WithSymbolTable()
{
	// In lieu of a preference (since there is no preference support on the server 
	// currently), we're going to check for the presence of a file next to the application
	// called "NoSymbolTable".  If the file is present, we're not going to let anyone get
	// a symbol table to work with.
	bool result = true;

	VFolder* execfold = VFolder::RetainSystemFolder( eFK_Executable, false);
	if (execfold != nil)
	{
		VFile ff(*execfold, L"NoSymbolTable.txt");
		if (ff.Exists())
			result = false;
		execfold->Release();
	}
	return result;
}


VError VProject::OpenSymbolTable()
{
	VError err = VE_OK;

	if ((fSymbolTable == NULL) && _WithSymbolTable())
	{
	#if VERSIONDEBUG
		VString lName;
		fProjectItem->GetDisplayName( lName);
		lName = "\"" + lName + "\"";
		DebugMsg( L"Open symbol table of project " + lName + L"\n");
	#endif

		CLanguageSyntaxComponent *languageEngine = VComponentManager::RetainComponentOfType< CLanguageSyntaxComponent >();
		if (languageEngine)
		{
			fSymbolTable = languageEngine->CreateSymbolTable();
			if (fSymbolTable != NULL)
			{
				// Now that we have the newly-created symbol table, we want to open it up
				// so that it is ready for use
				VFilePath folderPath;
				GetProjectFolderPath( folderPath);
				VString fileName;
				#if RIA_SERVER
					// The server gets a different file name for the symbol table than the studio, so that there
					// can't be any conflicts between the two when the same project is opened at the same time.
					fileName = "ServerSymbolTable.";
				#else
					fileName = "SymbolTable.";
				#endif
				fileName += RIAFileKind::kSymbolFileExtension;
				VFilePath databaseFilePath = folderPath.ToSubFile( fileName );
				VFile databaseFile( databaseFilePath );
				if (!fSymbolTable->OpenSymbolDatabase( databaseFile ))
				{
					ReleaseRefCountable( &fSymbolTable);
				}
			}
			languageEngine->Release();
		}

		if (!fSymbolTable) 
		{
			err = VE_UNKNOWN_ERROR;

			if (GetSolution()->GetSolutionMessageManager() != NULL)
			{
				VString localizedMessage;
				GetSolution()->GetSolutionMessageManager()->GetLocalizedStringFromResourceName(kSOMA_COULD_NOT_OPEN_THE_SYMBOL_DATABASE, localizedMessage);
				GetSolution()->GetSolutionMessageManager()->DisplayMessage(localizedMessage);
			}
		}
	}

	return err;
}


void VProject::CloseSymbolTable()
{
#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Close symbol table of project " + lName + L"\n");
#endif

	ReleaseRefCountable( &fSymbolTable);
}


ISymbolTable *VProject::GetSymbolTable()
{
	return fSymbolTable;
}


VSolution* VProject::GetSolution()
{
	if (fSolution == NULL)
		fSolution = fProjectItem->GetParent()->GetBehaviour()->GetSolution(); 

	assert(fSolution != NULL);

	return fSolution;
}

VError VProject::Load()
{
	VError err = VE_OK;

	VFilePath folderPath;
	fProjectItem->GetURL().GetFilePath(folderPath);
	VFolder folder = VFolder(folderPath);

#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Load project " + lName + L"\n");
#endif

		if (fSolution->GetDelegate() != NULL)
			fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

		SynchronizeWithFileSystem( fProjectItem);

		if (fProjectItemProjectFile == NULL)
		{
			xbox_assert(false);

			VString projectName;
			fProjectItem->GetDisplayName( projectName);
			
			VFilePath projectFilePath( folderPath);
			projectFilePath.SetFileName( projectName, false);
			projectFilePath.SetExtension( RIAFileKind::kProjectFileExtension);
			
			VProjectItem* projectItemProjectFile = fProjectItem->NewChild( VURL(projectFilePath), VProjectItem::eFILE);
			if (projectItemProjectFile->ContentExists())
			{
				projectItemProjectFile->Release();
				VString fullPath;
				projectFilePath.GetPath( fullPath);
				projectItemProjectFile = GetProjectItemFromFullPath( fullPath);
				if (projectItemProjectFile != NULL)
					SetProjectItemProjectFile(projectItemProjectFile);
				else
					err = VE_UNKNOWN_ERROR;
			}
			else
			{
				if (projectItemProjectFile->CreatePhysicalItem())
				{
					// enregistrer le projectItem dans la map des pathes du projet
					RegisterProjectItemInMapOfFullPathes(projectItemProjectFile);
					SetProjectItemProjectFile(projectItemProjectFile);
				}
				else
				{
					projectItemProjectFile->Release();
					err = VE_UNKNOWN_ERROR;
				}
			}
		}

		if (err == VE_OK)
		{
			err = _LoadProjectFile();
		}

		if (err == VE_OK)
		{
			if (fSourceControlConnector && (fSourceControlConnector->GetSourceControl()) && (fSourceControlConnector->IsConnectedToSourceControl()))
				_LoadSCCStatusOfProjectItems();
		}

		if (err == VE_OK)
		{
			err = OpenSymbolTable();
		}

		if (fSolution->GetDelegate() != NULL)
			fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();

	return err;
}


VError VProject::Unload()
{
#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Unload project " + lName + L"\n");
#endif

	xbox_assert(!fIsWatchingFileSystem);
	xbox_assert(!fIsUpdatingSymbolTable);

	StopBackgroundDeleteFiles();

	CloseSymbolTable();

	return VE_OK;
}


VError VProject::DoProjectWillClose()
{
	VError err = VE_OK;

	if (fProjectFileDirtyStamp > 0)
	{
		if (fSolution->GetDelegate() != NULL)
		{
			e_Save_Action saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_BeforeClosing, true);

			if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
			{
				if (saveAction == e_SAVE)
					err = _SaveProjectFile();
			}
			else
			{
				err = VE_USER_ABORT;
			}
		}
	}

	return err;
}


VError VProject::Close()
{
	return VE_OK;
}


VError VProject::StartWatchingFileSystem()
{
	VFilePath path;
	if (!GetProjectFolderPath( path))
		return VE_UNKNOWN_ERROR;

	VFolder folder( path);

#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Start watching file system for project " + lName + L"\n");
#endif

	fIsWatchingFileSystem = true;

	return VFileSystemNotifier::Instance()->StartWatchingForChanges( folder, VFileSystemNotifier::kAll, this, 100 );
}


VError VProject::StopWatchingFileSystem()
{
	VFilePath path;
	if (!GetProjectFolderPath( path))
		return VE_UNKNOWN_ERROR;

	VFolder folder( path);

#if VERSIONDEBUG
	VString lName;
	fProjectItem->GetDisplayName( lName);
	lName = "\"" + lName + "\"";
	DebugMsg( L"Stop watching file system for project " + lName + L"\n");
#endif

	fIsWatchingFileSystem = false;

	return VFileSystemNotifier::Instance()->StopWatchingForChanges( folder, this );
}


VError VProject::ReloadCatalogs()
{
	VError err = VE_OK;

	VectorOfProjectItems projectItemsVector;
	fProjectItem->GetProjectItemsVector(VProjectItem::eCATALOG_FILE, projectItemsVector, false);

	for (VectorOfProjectItemsIterator i = projectItemsVector.begin(); 
		i != projectItemsVector.end(); ++i)
	{
		err = GetSolution()->ReloadCatalog( *i);
	}
			
	return err;
}


void VProject::LoadOrSaveProjectUserFile(bool inLoad)
{
	if (fSourceControlConnector)
	{
		VString strProjectPath;
		VFilePath strProjectVFilePath;
		fProjectItem->GetURL().GetFilePath(strProjectVFilePath);
		strProjectVFilePath.GetPath(strProjectPath);
		VString strProjectName;
		fProjectItem->GetDisplayName(strProjectName);
		strProjectPath += strProjectName;
		strProjectPath += ".";
		VString strProjectUserFilePath = strProjectPath;
		VString userName;
		VSystem::GetLoginUserName(userName);
		strProjectUserFilePath += userName;
		VFile userFile(strProjectUserFilePath);
		(inLoad) ? fProjectUser->LoadFromUserFile(userFile) : fProjectUser->SaveToUserFile(userFile);
	}
}


bool VProject::ConnectToSourceControl(XBOX::VString& inSourceControlId)
{
	bool isConnectedToSourceControl = false;

	if (fSourceControlConnector)
	{
		isConnectedToSourceControl = fSourceControlConnector->ConnectToSourceControl(inSourceControlId);
	}

	return isConnectedToSourceControl;
}


ISourceControl* VProject::GetSourceControl() const
{
	return fSourceControlConnector ? fSourceControlConnector->GetSourceControl() : NULL;
} 


XBOX::VString VProject::GetSourceControlID() const
{
	return fSourceControlConnector ? fSourceControlConnector->GetSourceControlID() : "";
}


bool VProject::IsConnectedToSourceControl() const
{
	return fSourceControlConnector ? fSourceControlConnector->IsConnectedToSourceControl() : false;
}


void VProject::_LoadSCCStatusOfProjectItems()
{
	VectorOfProjectItems projectItems;
	VProjectItemTools::GetChildFile( fProjectItem, projectItems, true);
	_LoadSCCStatusOfProjectItems(projectItems);
}

VError VProject::_LoadSCCStatusOfProjectItems(const VectorOfProjectItems& inProjectItems)
{
	VError err = VE_OK;

	assert(inProjectItems.size() > 0);

	if (inProjectItems.size() > 0)
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				if ((*it)->GetFilePath(vfilepath))
					vFilesFullPath.push_back(vfilepath);
			}

			std::vector<ESCCStatus> infos;
			infos.resize(inProjectItems.size());

			if (!fSourceControlConnector->GetSourceControl()->QueryInfo(vFilesFullPath, infos))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				std::vector<ESCCStatus>::iterator it_infos = infos.begin();
				for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
					it != inProjectItems.end(); ++it, ++it_infos)
				{
					// TO DO 280309 ce code sera a adapter lorsque l'on aura (enfin) les specs...
					// attention a l'ordre
					if ((*it_infos) & kSccStateDeleted)
						(*it)->SetSCCStatus(VProjectItem::eDELETED_FILE);
					else if ((*it_infos) & kSccStateCheckedOut)
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_OUT_TO_ME);
					else if ((*it_infos) & kSccStateOutOther)
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_OUT_BY_SOMENONE);
					else if ((*it_infos) & kSccStateControlled)  // ???????????
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_IN);
					else if ((*it_infos) & kSccStateOutOfdate)  
						(*it)->SetSCCStatus(VProjectItem::eOUT_OF_DATE);

					/*if (infos & kSccStateCheckedOut)
						(*it)->SetState(VProjectItem::eNEWLY_ADDED_FILE);*/
				}
			}
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::_LoadSCCStatusOfProjectItems(const VectorOfProjectItems& inProjectItems, std::vector<VFilePath>& inFileFullPathes)
{
	VError err = VE_OK;

	assert(inFileFullPathes.size() > 0);
	assert(inProjectItems.size() == inFileFullPathes.size());

	if (inFileFullPathes.size() > 0)
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			std::vector<ESCCStatus> infos;
			infos.resize(inFileFullPathes.size());

			if (!fSourceControlConnector->GetSourceControl()->QueryInfo(inFileFullPathes, infos))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				std::vector<ESCCStatus>::iterator it_infos = infos.begin();
				for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
					it != inProjectItems.end(); ++it, ++it_infos)
				{
					// TO DO 280309 ce code sera a adapter lorsque l'on aura (enfin) les specs...
					// attention a l'ordre
					if ((*it_infos) & kSccStateDeleted)
						(*it)->SetSCCStatus(VProjectItem::eDELETED_FILE);
					else if ((*it_infos) & kSccStateCheckedOut)
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_OUT_TO_ME);
					else if ((*it_infos) & kSccStateOutOther)
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_OUT_BY_SOMENONE);
					else if ((*it_infos) & kSccStateControlled)  // ???????????
						(*it)->SetSCCStatus(VProjectItem::eCHECKED_IN);
					else if ((*it_infos) & kSccStateOutOfdate)  
						(*it)->SetSCCStatus(VProjectItem::eOUT_OF_DATE);

					/*if (infos & kSccStateCheckedOut)
						(*it)->SetState(VProjectItem::eNEWLY_ADDED_FILE);*/
				}
			}
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::AddToSourceControl(const VectorOfProjectItems& inProjectItems)
{
	VError err = VE_OK;

	assert(!inProjectItems.empty());

	if (!inProjectItems.empty())
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;
			std::vector<sLONG> vFilesType;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				if ((*it)->IsPhysicalFile())
				{
					if ((*it)->GetFilePath(vfilepath))
					{
						vFilesFullPath.push_back(vfilepath);
						vFilesType.push_back(SCC_FILETYPE_TEXT);
					}
				}
			}

			if (!fSourceControlConnector->GetSourceControl()->Add(vFilesFullPath, vFilesType))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				_LoadSCCStatusOfProjectItems(inProjectItems, vFilesFullPath);
				if (fSolution->GetDelegate() != NULL)
					fSolution->GetDelegate()->DoSourceControlStatusChanged( inProjectItems);
			}

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::GetLatestVersion(const VectorOfProjectItems& inProjectItems)
{
	VError err = VE_OK;

	assert(!inProjectItems.empty());

	if (!inProjectItems.empty())
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				if ((*it)->GetFilePath(vfilepath))
					vFilesFullPath.push_back(vfilepath);
			}

			if (!fSourceControlConnector->GetSourceControl()->Get(vFilesFullPath))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				_LoadSCCStatusOfProjectItems(inProjectItems, vFilesFullPath);
				if (fSolution->GetDelegate() != NULL)
					fSolution->GetDelegate()->DoSourceControlStatusChanged( inProjectItems);
			}

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::CheckOut(const VectorOfProjectItems& inProjectItems, bool inCheckOutProjectFiles)
{
	VError err = VE_OK;

	assert(!inProjectItems.empty());

	if (!inProjectItems.empty())
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			VectorOfProjectItems filteredProjectItems;
			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				// filtre :
				if ((!inCheckOutProjectFiles) && (*it)->ConformsTo( RIAFileKind::kProjectFileKind))
					continue;
				filteredProjectItems.push_back(*it);
				if ((*it)->GetFilePath(vfilepath))
					vFilesFullPath.push_back(vfilepath);
			}

			if (vFilesFullPath.size() > 0)
			{
				if (!fSourceControlConnector->GetSourceControl()->CheckOut(vFilesFullPath))
					err = VE_UNKNOWN_ERROR;		
				else
				{
					_LoadSCCStatusOfProjectItems(filteredProjectItems, vFilesFullPath);
					if (fSolution->GetDelegate() != NULL)
						fSolution->GetDelegate()->DoSourceControlStatusChanged( inProjectItems);
				}
			}

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::CheckIn(const VectorOfProjectItems& inProjectItems)
{
	VError err = VE_OK;

	assert(inProjectItems.size() > 0);

	if (inProjectItems.size() > 0)
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				if ((*it)->GetFilePath(vfilepath))
					vFilesFullPath.push_back(vfilepath);
			}

			if (!fSourceControlConnector->GetSourceControl()->CheckIn(vFilesFullPath))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				_LoadSCCStatusOfProjectItems(inProjectItems, vFilesFullPath);
				if (fSolution->GetDelegate() != NULL)
					fSolution->GetDelegate()->DoSourceControlStatusChanged( inProjectItems);
			}

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

VError VProject::Revert(const VectorOfProjectItems& inProjectItems)
{
	VError err = VE_OK;

	assert(inProjectItems.size() > 0);

	if (inProjectItems.size() > 0)
	{
		if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
		{
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			VFilePath vfilepath;
			std::vector<VFilePath> vFilesFullPath;

			for (VectorOfProjectItemsConstIterator it = inProjectItems.begin(); 
				it != inProjectItems.end(); ++it)
			{
				if ((*it)->GetFilePath(vfilepath))
					vFilesFullPath.push_back(vfilepath);
			}

			if (!fSourceControlConnector->GetSourceControl()->Revert(vFilesFullPath))
				err = VE_UNKNOWN_ERROR;		
			else
			{
				_LoadSCCStatusOfProjectItems(inProjectItems, vFilesFullPath);
				if (fSolution->GetDelegate() != NULL)
					fSolution->GetDelegate()->DoSourceControlStatusChanged( inProjectItems);
			}

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();
		}
		else	// pas de vscc
			err = VE_UNKNOWN_ERROR;		
	}

	return err;
}

ISourceControlInterface* VProject::GetSCCInterfaceForCodeEditor()
{
	ISourceControlInterface* result = NULL;

	if(fSourceControlConnector != NULL)
	{
		result = fSourceControlConnector->GetSCCInterfaceForCodeEditor();
	}

	return result;
}


VError VProject::SynchronizeWithFileSystem( VProjectItem *inItem)
{
#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->EnterCall( PROFILER_CALL_VProject_SynchronizeWithFileSystem);
#endif

	VError err = VE_OK;

	if (fSolution->GetDelegate() != NULL)
		fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

	VProjectFileStampSaver stampSaver(this);
	
	err = _SynchronizeWithFileSystem( inItem);

	// Reprocess the files in the project so that the symbol table stays in sync.
	BackgroundParseFiles();

	if (fSolution->GetDelegate() != NULL)
		fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();

	if ((err == VE_OK) && stampSaver.StampHasBeenChanged())
	{
		e_Save_Action saveAction = e_SAVE;

		if (fSolution->GetDelegate() != NULL)
			saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_SynchronizeWithFileSystem, false);

		if (saveAction == e_SAVE)		
			_SaveProjectFile();
	}

#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->ExitCall( PROFILER_CALL_VProject_SynchronizeWithFileSystem);
#endif

	return err;
}


void VProject::RegisterProjectItemAndChildrenInMapOfFullPathes(VProjectItem* inProjectItem, bool inRegisterChildren)
{
	if (inRegisterChildren)
	{
		for (VProjectItemIterator it(inProjectItem); it.IsValid(); ++it)
		{
			RegisterProjectItemAndChildrenInMapOfFullPathes(it, true);
		}
	}
	RegisterProjectItemInMapOfFullPathes(inProjectItem);
}

void VProject::RegisterProjectItemInMapOfFullPathes(VProjectItem* inProjectItem)
{
#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->EnterCall( PROFILER_CALL_VProject_RegisterProjectItemInMapOfFullPathes);
#endif

	if (inProjectItem->HasFilePath())
	{
		VFilePath filePath;
		inProjectItem->GetFilePath(filePath);
		VString strProjectItemFullPath = filePath.GetPath();
		if (!strProjectItemFullPath.IsEmpty())
		{
		#if VERSION_LINUX
			// sc & jmo 07/07/2011, WAK0072129
			PathBuffer tmpBuf;
			VError verr = tmpBuf.Init( strProjectItemFullPath, PathBuffer::withRealPath);
			verr = tmpBuf.ToPath( &strProjectItemFullPath);
		#endif

			fMapFullPathProjectItem[strProjectItemFullPath] = inProjectItem;
		}
		else
		{
			assert(false);
		}
	}

#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->ExitCall( PROFILER_CALL_VProject_RegisterProjectItemInMapOfFullPathes);
#endif

}

void VProject::UnregisterProjectItemAndChildrenFromMapOfFullPathes(VProjectItem* inProjectItem, bool inRegisterChildren)
{
	if (inRegisterChildren)
	{
		for (VProjectItemIterator it(inProjectItem); it.IsValid(); ++it)
		{
			UnregisterProjectItemAndChildrenFromMapOfFullPathes(it, true);
		}
	}

	UnregisterProjectItemFromMapOfFullPathes(inProjectItem);
}

void VProject::UnregisterProjectItemFromMapOfFullPathes(VProjectItem* inProjectItem)
{
	if (inProjectItem->HasFilePath())
	{
		VFilePath filePath;
		inProjectItem->GetFilePath(filePath);
		VString strProjectItemFullPath = filePath.GetPath();
		if (!strProjectItemFullPath.IsEmpty())
		{
			VProjectItem* projectItem = GetProjectItemFromFullPath(strProjectItemFullPath);
			if (projectItem)
			{
#if RIA_STUDIO
				RemoveProjectItemFromSymbolTable( projectItem );
#endif
				fMapFullPathProjectItem.erase(strProjectItemFullPath);
			}
		}
		else
			assert(false);
	}
}

VProjectItem* VProject::GetProjectItemFromFullPath( const VString& inFullPath) const
{
#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->EnterCall( PROFILER_CALL_VProject_GetProjectItemFromFullPath);
#endif

	VString fullPath( inFullPath);

#if VERSION_LINUX
	// sc & jmo 07/07/2011, WAK0072129
	PathBuffer tmpBuf;
	VError verr = tmpBuf.Init( fullPath, PathBuffer::withRealPath);
	verr = tmpBuf.ToPath( &fullPath);
#endif

	MapFullPathProjectItem::const_iterator i = fMapFullPathProjectItem.find(fullPath);

#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->ExitCall( PROFILER_CALL_VProject_GetProjectItemFromFullPath);
#endif

	return (i == fMapFullPathProjectItem.end()) ? NULL : i->second;
}


VProjectSettings* VProject::RetainSettings( XBOX::VError& outError) const
{
	outError = VE_OK;
	
	VProjectSettings *settings = new VProjectSettings();

	if (settings != NULL)
	{
		VectorOfProjectItems itemsVector;

		GetProjectItemsFromTag( kSettingTag, itemsVector);
		for (VectorOfProjectItemsIterator iter = itemsVector.begin() ; iter != itemsVector.end() && outError == VE_OK ; ++iter)
		{
			if (*iter != NULL)
			{
				VFilePath path;
				(*iter)->GetFilePath( path);
				outError = settings->AppendAndLoadSettingsFile( path);
			}
		}
	}
	else
	{
		outError = VE_MEMORY_FULL;
	}

	return settings;
}


void VProject::TagItemAs( VProjectItem *inProjectItem, const VProjectItemTag& inTag)
{
	if (testAssert(VProjectItemTagManager::Get()->IsRegisteredProjectItemTag( inTag)))
	{
		xbox_assert(inTag != kIndexPageTag);

		if (inProjectItem != NULL && !inProjectItem->HasTag( inTag))
		{
			e_Save_Action saveAction = e_SAVE;

			if (fSolution->GetDelegate() != NULL)
				saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_TagItem, true);

			if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
			{
				VProjectFileStampSaver stampSaver(this);

				VectorOfProjectItems changedItems;

				EProjectItemTagProperties projectItemTagProperties = VProjectItemTagManager::Get()->GetProperties( inTag);
				bool singleProjectItem = ((projectItemTagProperties & ePITP_ApplyToSingleFile) != 0) || ((projectItemTagProperties & ePITP_ApplyToSingleFolder) != 0);

				if (singleProjectItem)
				{
					for (VectorOfProjectItems::iterator iter = fReferencedItems.begin() ; iter != fReferencedItems.end() ; ++iter)
					{
						VProjectItem *item = *iter;
						if (singleProjectItem && item->HasTag( inTag))
						{
							item->RemoveTag( inTag);
							changedItems.push_back( item);
						}
					}

					for (VectorOfProjectItems::iterator iter = changedItems.begin() ; iter != changedItems.end() ; ++iter)
					{
						if (!(*iter)->IsTagged() && !(*iter)->IsExternalReference())
						{
							_UnreferenceItem( *iter, true);
							(*iter)->Touch();
						}
					}
				}

				inProjectItem->AddTag( inTag);
				_ReferenceItem( inProjectItem, true);
				inProjectItem->Touch();
				changedItems.push_back( inProjectItem);

				if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged()) 
					_SaveProjectFile();
			}
		}
	}
}


void VProject::RemoveTagFromItem( VProjectItem *inProjectItem, const VProjectItemTag& inTag)
{
	if (testAssert(VProjectItemTagManager::Get()->IsRegisteredProjectItemTag( inTag)))
	{
		if (inProjectItem != NULL && inProjectItem->HasTag( inTag))
		{
			e_Save_Action saveAction = e_SAVE;

			if (fSolution->GetDelegate() != NULL)
				saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_UntagItem, true);

			if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
			{
				VProjectFileStampSaver stampSaver(this);

				inProjectItem->RemoveTag( inTag);

				if (!inProjectItem->IsTagged() && !inProjectItem->IsExternalReference())
				{
					_UnreferenceItem( inProjectItem, true);
					inProjectItem->Touch();
				}

				if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
					_SaveProjectFile();
			}
		}
	}
}


void VProject::RemoveAllTagsFromItem( VProjectItem *inProjectItem)
{
	if (inProjectItem != NULL && inProjectItem->IsTagged())
	{
		e_Save_Action saveAction = e_SAVE;

		if (fSolution->GetDelegate() != NULL)
			saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_UntagItem, true);

		if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
		{
			VProjectFileStampSaver stampSaver(this);

			inProjectItem->RemoveAllTags();

			if (!inProjectItem->IsExternalReference())
			{
				_UnreferenceItem( inProjectItem, true);
				inProjectItem->Touch();
			}

			if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
				_SaveProjectFile();
		}
	}
}


void VProject::GetCompatibleTags( const VProjectItem* inProjectItem, std::vector<VProjectItemTag>& outTags) const
{
	outTags.clear();

	if (inProjectItem != NULL)
	{
		VFile *file = NULL;

		std::vector<VProjectItemTag> tags;
		VProjectItemTagManager::Get()->GetProjectItemTagCollection( tags);
		for (std::vector<VProjectItemTag>::iterator iter = tags.begin() ; iter != tags.end() ; ++iter)
		{
			EProjectItemTagProperties projectItemTagProperties = VProjectItemTagManager::Get()->GetProperties( *iter);
			bool applyToFiles = ((projectItemTagProperties & ePITP_ApplyToSingleFile) != 0) || ((projectItemTagProperties & ePITP_ApplyToMultipleFiles) != 0);
			bool applyToFolders = ((projectItemTagProperties & ePITP_ApplyToSingleFolder) != 0) || ((projectItemTagProperties & ePITP_ApplyToMultipleFolders) != 0);
			
			if (applyToFolders && inProjectItem->GetKind() == VProjectItem::eFOLDER && !inProjectItem->IsExternalReference())
			{
				outTags.push_back( *iter);
			}
			else if (applyToFiles && inProjectItem->IsPhysicalFile() && !inProjectItem->IsExternalReference())
			{
				VFileKind *fileKind = VProjectItemTagManager::Get()->RetainDefaultFileKind( *iter);
				if (fileKind != NULL)
				{
					if (file == NULL)
					{
						VFilePath path;
						inProjectItem->GetFilePath( path);
						file = new VFile( path);
					}

					if (file != NULL)
					{
						if (file->ConformsTo( *fileKind))
						{
							outTags.push_back( *iter);
						}
					}
				}
				ReleaseRefCountable( &fileKind);
			}
		}
		ReleaseRefCountable( &file);
	}
}


void VProject::GetProjectItemsFromTag( const VProjectItemTag& inTag, VectorOfProjectItems& outProjectItems) const
{
	outProjectItems.clear();

	if (!VProjectItemTagManager::Get()->IsRegisteredProjectItemTag( inTag))
		return;

	xbox_assert( inTag != kIndexPageTag);

	bool done = false;

	EProjectItemTagProperties projectItemTagProperties = VProjectItemTagManager::Get()->GetProperties( inTag);
	bool singleProjectItem = ((projectItemTagProperties & ePITP_ApplyToSingleFile) != 0) || ((projectItemTagProperties & ePITP_ApplyToSingleFolder) != 0);
	bool applyToFiles = ((projectItemTagProperties & ePITP_ApplyToSingleFile) != 0) || ((projectItemTagProperties & ePITP_ApplyToMultipleFiles) != 0);
	bool applyToFolders = ((projectItemTagProperties & ePITP_ApplyToSingleFolder) != 0) || ((projectItemTagProperties & ePITP_ApplyToMultipleFolders) != 0);
	bool applyToFolderContent = (projectItemTagProperties & ePITP_ApplyToFolderContent) != 0;
	VectorOfProjectItems defaultFolders;

	// First, search through the referenced items.
	for (VectorOfProjectItemsConstIterator iter = fReferencedItems.begin() ; iter != fReferencedItems.end() && !done ; ++iter)
	{
		VProjectItem *item = *iter;
		if (item->HasTag( inTag))
		{
			if (item->IsPhysicalFile())
			{
				if (std::find( outProjectItems.begin(), outProjectItems.end(), item) == outProjectItems.end())
				{
					outProjectItems.push_back( item);
					if (singleProjectItem)
						done = true;
				}
			}
			else if(item->IsPhysicalFolder())
			{
				if (applyToFolderContent)
				{
					if (std::find( outProjectItems.begin(), outProjectItems.end(), item) == outProjectItems.end())
						defaultFolders.push_back( item);
				}
				else
				{
					if (std::find( outProjectItems.begin(), outProjectItems.end(), item) == outProjectItems.end())
					{
						outProjectItems.push_back( item);
						if (singleProjectItem)
							done = true;
					}
				}
			}
		}
	}

	if (!done && inTag == kWebFolderTag)
	{
		// compatibility note: before be a tagged item, the web folder was defined in the web app service settings
		VError err = VE_OK;

		VProjectSettings *settings = RetainSettings( err );
		if (err == VE_OK && settings != NULL)
		{
			VValueBag *webAppSettings = settings->RetainSettings( RIASettingID::webApp);
			if (webAppSettings != NULL)
			{
				VFilePath projectFolderPath;
				GetProjectFolderPath( projectFolderPath);

				VString webFolder = RIASettingsKeys::WebApp::documentRoot.Get( webAppSettings);
				if (!webFolder.IsEmpty() && webFolder[webFolder.GetLength() - 1] != CHAR_SOLIDUS)
					webFolder += CHAR_SOLIDUS;

				VFilePath path( projectFolderPath, webFolder, FPS_POSIX);
				VProjectItem *item = GetProjectItemFromFullPath( path.GetPath());
				if (item != NULL)
				{
					outProjectItems.push_back( item);
					done = true;
				}
			}
			ReleaseRefCountable( &webAppSettings);
		}
		ReleaseRefCountable( &settings);
	}

	if (!done)
	{
		// Search into default folders: default folders are defined by a posix path relative to the project folder
		VectorOfVString defaultFoldersPaths;
		if (VProjectItemTagManager::Get()->GetDefaultFolders( inTag, defaultFoldersPaths))
		{
			// Build the list of default folders
			for (VectorOfVString::iterator iter = defaultFoldersPaths.begin() ; iter != defaultFoldersPaths.end() && !done ; ++iter)
			{
				VString strDefaultFolderRelativePath( *iter), strDefaultFolderFullPath;
				VFilePath defaultFolderFilePath;

				if (!strDefaultFolderRelativePath.IsEmpty() && (strDefaultFolderRelativePath[strDefaultFolderRelativePath.GetLength()-1] != POSIX_FOLDER_SEPARATOR))
					strDefaultFolderRelativePath += POSIX_FOLDER_SEPARATOR;

				BuildPathFromRelativePath( defaultFolderFilePath, strDefaultFolderRelativePath, FPS_POSIX);
				defaultFolderFilePath.GetPath( strDefaultFolderFullPath);
				VProjectItem *projectItem = GetProjectItemFromFullPath( strDefaultFolderFullPath);
				if (projectItem != NULL)
				{
					defaultFolders.push_back( projectItem);
				}
			}

			VFileKind *fileKind = VProjectItemTagManager::Get()->RetainDefaultFileKind( inTag);

			for (VectorOfProjectItemsIterator iter = defaultFolders.begin() ; iter != defaultFolders.end() && !done ; ++iter)
			{
				if (applyToFolderContent)
				{
					for (VProjectItemIterator itemsIter( *iter) ; itemsIter.IsValid() && !done ; ++itemsIter)
					{
						if (itemsIter->IsPhysicalFile())
						{
							if (applyToFiles && (fileKind != NULL))
							{
								VFilePath path;
								if (itemsIter->GetFilePath( path))
								{
									VFile file (path);
									if (file.ConformsTo( *fileKind))
									{
										if (std::find( outProjectItems.begin(), outProjectItems.end(), (VProjectItem*) itemsIter) == outProjectItems.end())
										{
											outProjectItems.push_back( itemsIter);
											if (singleProjectItem)
												done = true;
										}
									}
								}
							}
						}
						else if (itemsIter->IsPhysicalFolder())
						{
							if (applyToFolders)
							{
								if (std::find( outProjectItems.begin(), outProjectItems.end(), (VProjectItem*) itemsIter) == outProjectItems.end())
								{
									outProjectItems.push_back( itemsIter);
									if (singleProjectItem)
										done = true;
								}
							}
						}
					}
				}
				else if (applyToFolders)
				{
					if (std::find( outProjectItems.begin(), outProjectItems.end(), *iter) == outProjectItems.end())
					{
						outProjectItems.push_back( *iter);
						if (singleProjectItem)
							done = true;
					}
				}
			}

			ReleaseRefCountable( &fileKind);
		}
	}

	if (!done)
	{
		// Search for default files: default files are defined by a posix path relative to the project folder
		VectorOfVString defaultFiles;
		if (VProjectItemTagManager::Get()->GetDefaultFiles( inTag, defaultFiles))
		{
			for (VectorOfVString::iterator iter = defaultFiles.begin() ; iter != defaultFiles.end() && !done ; ++iter)
			{
				VFilePath defaultFilePath;

				VString strDefaultFileRelativePath( *iter);
				BuildPathFromRelativePath( defaultFilePath, strDefaultFileRelativePath, FPS_POSIX);

				VString  strDefaultFileFullPath;
				defaultFilePath.GetPath( strDefaultFileFullPath);
				VProjectItem *projectItem = GetProjectItemFromFullPath( strDefaultFileFullPath);
				if (projectItem != NULL)
				{
					if (std::find( outProjectItems.begin(), outProjectItems.end(), projectItem) == outProjectItems.end())
					{
						outProjectItems.push_back( projectItem);
						if (singleProjectItem)
							done = true;
					}
				}
			}
		}
	}
}


VProjectItem* VProject::GetProjectItemFromTag( const VProjectItemTag& inTag) const
{
	VectorOfProjectItems itemsVector;
	GetProjectItemsFromTag( inTag, itemsVector);
	return (!itemsVector.empty()) ? itemsVector[0] : NULL;
}


VProjectItem* VProject::GetMainPage()
{
	VProjectItem *mainPage = NULL;
	VError err = VE_OK;

	VProjectSettings *settings = RetainSettings( err);

	if (err == VE_OK && settings != NULL)
	{
		VProjectItem *webFolder = GetProjectItemFromTag( kWebFolderTag);
		if (webFolder != NULL)
		{
			VFilePath path;
			VString directoryIndex;

			webFolder->GetFilePath( path);
			settings->GetDirectoryIndex( directoryIndex);	// sc 23/02/2012
			path.SetFileName( directoryIndex, true);
			mainPage = GetProjectItemFromFullPath( path.GetPath());
		}
	}
	
	ReleaseRefCountable( &settings);
	
	return mainPage;
}


VError VProject::BuildFileURLString(VString &outFileURL, VProjectItem* inProjectItem)
{
	VString hostName, ip, pattern;
	sLONG port = -1;

	outFileURL.Clear();
	
	VError err = GetPublicationSettings( hostName, ip, port, pattern);	// sc 06/01/2010 factorisation
	if (err == VE_OK)
	{
		outFileURL = "http://127.0.0.1";

		if (port != 80)
		{
			VString strPort;
			strPort.FromLong(port);
			outFileURL += ":";
			outFileURL += strPort;
		}

		if (!pattern.IsEmpty())
		{
			outFileURL += "/";
			outFileURL += pattern;
		}

		if (inProjectItem != NULL)
		{
			VProjectItem *webFolder = GetProjectItemFromTag( kWebFolderTag);
			if (webFolder != NULL)
			{
				VFilePath webFolderPath, itemPath;
				if (webFolder->GetFilePath( webFolderPath) && inProjectItem->GetFilePath( itemPath))
				{
					VString relativePath;
					if (itemPath.GetRelativePosixPath( webFolderPath, relativePath))
					{
						if ( outFileURL [ outFileURL. GetLength ( ) - 1 ] != '/')
							outFileURL += '/';
						outFileURL += relativePath;
					}
					else
					{
						err = VE_RIA_FILE_DOES_NOT_EXIST;
					}

				}
			}
		}
	}

	return err;
}

VError VProject::BuildAppliURLString(VString &outAppliURL, bool inWithIndexPage)
{
	VError err = VE_OK;

	if (inWithIndexPage)
	{
		VProjectItem *indexPage = GetMainPage();
		if (indexPage != NULL)
			err = BuildFileURLString( outAppliURL, indexPage);
		else
			err = VE_RIA_FILE_DOES_NOT_EXIST;
	}
	else
	{
		err = BuildFileURLString( outAppliURL, NULL);
	}

	return err;
}

XBOX::VError VProject::GetPublicationSettings( XBOX::VString& outHostName, XBOX::VString& outIP, sLONG& outPort, XBOX::VString& outPattern) const
{
	VError vError = VE_OK;
	VProjectSettings *settings = RetainSettings( vError );

	outHostName.Clear();
	outIP.Clear();
	outPort = -1;
	outPattern.Clear();
	
	if (vError == VE_OK && settings != NULL)
	{
		if (!settings->HasProjectSettings())
		{
			vError = VE_RIA_CANNOT_LOAD_PROJECT_SETTINGS;
		}
		else
		{
			settings->GetHostName( outHostName);
			settings->GetListeningAddress( outIP);
		#if 0	// sc 25/03/2011 disable project pattern support
			settings->GetPattern( outPattern);
		#endif
		}
	}

	if ( vError == VE_OK )
	{
		if (!settings->HasHTTPServerSettings())
		{
			vError = VE_RIA_CANNOT_LOAD_HTTP_SETTINGS;
		}
		else
		{
			outPort = settings->GetListeningPort();
		}
	}

	ReleaseRefCountable( &settings);

	return vError;
}


VProjectItem* VProject::ReferenceExternalFolder( XBOX::VError& outError, VProjectItem *inParentItem, const XBOX::VURL& inURL)
{
	outError = VE_OK;
	VProjectItem *result = NULL;

	if (inParentItem != NULL)
	{
		assert(inParentItem->GetProjectOwner() == this);

		if (inParentItem->CheckAnyProjectItemParentIsExternalReference(true))
		{
			outError = VE_SOMA_INCLUSIVE_EXTERNAL_REFERENCE_NOT_AUTHORIZED;
		}
		else
		{
			e_Save_Action saveAction = e_SAVE;

			if (fSolution->GetDelegate() != NULL)
				saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_ReferenceExternalFolder, true);

			if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
			{
				result = inParentItem->NewChild( inURL, VProjectItem::eFOLDER);
				if (result != NULL)
				{
					VProjectFileStampSaver stampSaver(this);

					VFilePath path;
					inURL.GetFilePath( path);
					
					VString itemName;
					path.GetName( itemName);

					result->SetExternalReference( true);
					result->SetName( itemName);
					result->SetDisplayName( itemName);

					_ReferenceItem( result, true);
					RegisterProjectItemInMapOfFullPathes( result);
					
					if (fSolution->GetDelegate() != NULL)
						fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

					if (_SynchronizeWithFileSystem( result) == VE_OK)
						BackgroundParseFiles();
					
					if (fSolution->GetDelegate() != NULL)
						fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();

					if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
						_SaveProjectFile();
				}
				else
				{
					outError = VE_MEMORY_FULL;
				}
			}
		}
	}
	return result;
}


VProjectItem* VProject::ReferenceExternalFile( XBOX::VError& outError, VProjectItem *inParentItem, const XBOX::VURL& inURL)
{
	outError = VE_OK;
	VProjectItem *result = NULL;

	if (inParentItem != NULL)
	{
		assert(inParentItem->GetProjectOwner() == this);

		if (inParentItem->CheckAnyProjectItemParentIsExternalReference(true))
		{
			outError = VE_SOMA_INCLUSIVE_EXTERNAL_REFERENCE_NOT_AUTHORIZED;
		}
		else
		{
			e_Save_Action saveAction = e_SAVE;

			if (fSolution->GetDelegate() != NULL)
				saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_ReferenceExternalFile, true);

			if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
			{
				result = inParentItem->NewChild( inURL, VProjectItem::eFILE);
				if (result != NULL)
				{
					VProjectFileStampSaver stampSaver(this);

					VFilePath path;
					inURL.GetFilePath( path);
					
					VString itemName;
					path.GetName( itemName);

					result->SetExternalReference( true);
					result->SetName( itemName);
					result->SetDisplayName( itemName);

					_ReferenceItem( result, true);
					RegisterProjectItemInMapOfFullPathes( result);
					
					if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
						_SaveProjectFile();
				}
				else
				{
					outError = VE_MEMORY_FULL;
				}
			}
		}
	}
	return result;
}


VProjectItem* VProject::CreateFileItemFromPath( XBOX::VError& outError, const XBOX::VFilePath& inPath, bool inRecursive)
{
	return _CreateFileItemFromPath( outError, inPath, inRecursive, true);
}


VProjectItem* VProject::CreateFolderItemFromPath( XBOX::VError& outError, const XBOX::VFilePath& inPath, bool inRecursive, bool inSynchronizeWithFileSystem)
{
	return _CreateFolderItemFromPath( outError, inPath, inRecursive, inSynchronizeWithFileSystem, true);
}


XBOX::VError VProject::RemoveItem( VProjectItem *inProjectItem, bool inDeletePhysicalItems)
{
	VectorOfProjectItems items( 1, inProjectItem);
	return RemoveItems( items, inDeletePhysicalItems);
}


XBOX::VError VProject::RemoveItems( const VectorOfProjectItems& inProjectItems, bool inDeletePhysicalItems)
{
	VError err = VE_OK;

	e_Save_Action saveAction = e_SAVE;

	if (inDeletePhysicalItems && (fSolution->GetDelegate() != NULL) && _IsVectorContainsReferencedItems( inProjectItems, true))
		saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_RemoveItem, true);

	if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
	{
		VProjectItem *firstCommonParent = NULL;
		VectorOfProjectItems items( inProjectItems.begin(), inProjectItems.end());
		
		VProjectItemTools::RemoveUselessItems( items);
		firstCommonParent = VProjectItemTools::GetFirstCommonParent( items);

		for (VectorOfProjectItemsIterator itemIter = items.begin() ; itemIter != items.end() ; ++itemIter)
		{
			if ((*itemIter != fProjectItem) && (*itemIter != fProjectItemProjectFile))
			{
				if ((*itemIter)->IsExternalReference())
				{
					// TBD
					xbox_assert(false);
				}
				else if (inDeletePhysicalItems)
				{
					(*itemIter)->DeleteContent();
				}
			}
		}

		if (inDeletePhysicalItems)
		{
			VProjectFileStampSaver stampSaver(this);

			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoStartPossibleLongTimeOperation();

			if (_SynchronizeWithFileSystem( firstCommonParent) == VE_OK)
				BackgroundParseFiles();
						
			if (fSolution->GetDelegate() != NULL)
				fSolution->GetDelegate()->DoEndPossibleLongTimeOperation();

			if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
				_SaveProjectFile();
		}
	}

	return err;
}


XBOX::VError VProject::RenameItem( VProjectItem *inProjectItem, const XBOX::VString& inNewName)
{
	VError err = VE_OK;

	if (inProjectItem != NULL)
	{
		if ((inProjectItem == fProjectItem) || (inProjectItem == fProjectItemProjectFile))
		{
			VString name( inNewName);
			VString extension( L"." + RIAFileKind::kProjectFileExtension);
			if (name.EndsWith( extension))
				name.Truncate( name.GetLength() -  extension.GetLength());

			err = Rename( name);
		}
		else
		{
			VString name;
			inProjectItem->GetName( name);
			if (name != inNewName)
			{
				e_Save_Action saveAction = e_SAVE;
	
				if ((fSolution->GetDelegate() != NULL) && _IsItemReferenced( inProjectItem))
					saveAction = fSolution->GetDelegate()->DoActionRequireProjectFileSaving( fProjectItemProjectFile, ePA_RenameItem, true);

				if ((saveAction == e_SAVE) || (saveAction == e_NO_SAVE))
				{
					VProjectFileStampSaver stampSaver(this);

					_DoItemRemoved( inProjectItem, true);

					err = inProjectItem->RenameContent( inNewName);
					if (err == VE_OK)
						inProjectItem->Touch();

					_DoItemAdded( inProjectItem, true);

					if ((saveAction == e_SAVE) && stampSaver.StampHasBeenChanged())
						_SaveProjectFile();
				}
			}
		}
	}

	return err;
}


void VProject::_SetProjectItem( VProjectItem* inProjectItem)
{
	if (testAssert(fProjectItem == NULL && inProjectItem != NULL))
	{
		fProjectItem = RetainRefCountable( inProjectItem);

		VProjectItemProject *behaviour = dynamic_cast<VProjectItemProject*>(fProjectItem->GetBehaviour());
		if (testAssert(behaviour != NULL))
			behaviour->SetProject( this);

		RegisterProjectItemInMapOfFullPathes( fProjectItem);
	}
}


void VProject::_SetProjectFileItem( VProjectItem* inProjectItem)
{
	if (testAssert(fProjectItemProjectFile == NULL && inProjectItem != NULL))
	{
		fProjectItemProjectFile = RetainRefCountable( inProjectItem);
		RegisterProjectItemInMapOfFullPathes( fProjectItemProjectFile);

		if (fProjectItem != NULL)
			fProjectItem->AttachChild( fProjectItemProjectFile);
	}
}


VError VProject::_SynchronizeWithFileSystem( VProjectItem *inItem)
{
#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->EnterCall( PROFILER_CALL_VProject__SynchronizeWithFileSystem);
#endif

	VError err = VE_OK;

	// Synchronize the children and check for deleted items
	VectorOfProjectItems deletedItems;

	for (VProjectItemIterator iter(inItem) ; iter.IsValid() && (err == VE_OK) ; ++iter)
	{
		if (iter == fProjectItemProjectFile)
			continue;
		
		if (iter->IsGhost())
			continue;

		if (!iter->IsPhysicalLinkValid() && iter->ContentExists())
			iter->SetPhysicalLinkValid( true);	// sc 30/03/2012

		if (!iter->IsPhysicalLinkValid())	// sc 02/04/2012 WAK0076271, WAK0076272, WAK0076273
			continue;

		if (iter->IsPhysicalFileOrFolder())
		{
			if (!iter->ContentExists())
			{
				deletedItems.push_back( iter);
			}
			else
			{
				err = _SynchronizeWithFileSystem( iter);
			}
		}
	}

	VectorOfFilePathes deletedPathes;

	for (VectorOfProjectItemsIterator iter = deletedItems.begin() ; iter != deletedItems.end() ; ++iter)
	{
		VFilePath path;
		
		if ( (*iter)->GetFilePath(path) )
			deletedPathes.push_back( path );

		_DoItemRemoved( *iter, true);
	
		(*iter)->SetGhost(true);
		(*iter)->DeleteChildren();
	}
	
	if ( deletedPathes.size() && fSolution && fSolution->GetDelegate() )
		fSolution->GetDelegate()->DoProjectItemsDeleted( deletedPathes );

	for (VectorOfProjectItemsIterator iter = deletedItems.begin() ; iter != deletedItems.end() ; ++iter)
		(*iter)->Release();

	if (err == VE_OK)
	{
		// Check for new items
		if (inItem->IsPhysicalFolder())
		{
			VFilePath folderPath;
			if (inItem->GetFilePath( folderPath))
			{
				VFolder *folder = new VFolder(folderPath);
				if (folder != NULL)
				{
					for (VFolderIterator folderIter( folder, FI_WANT_FOLDERS | FI_WANT_INVISIBLES); folderIter.IsValid() && (err == VE_OK) ; ++folderIter)
					{
						VString folderName;
						folderIter->GetName( folderName);
						if (folderName == "__MACOSX")
							continue;

						if (folderName == "Internal Files")
							continue;

						// Check whether the child folder exists
						VString relativePath( folderName);
						relativePath += FOLDER_SEPARATOR;
						VProjectItem *folderItem = inItem->FindChildByRelativePath( relativePath);
						if (folderItem == NULL)
						{
							folderItem = inItem->NewChild( VProjectItem::eFOLDER);
							if (folderItem != NULL)
							{
								folderItem->SetName( folderName);
								folderItem->SetDisplayName( folderName);
								folderItem->SetRelativePath( relativePath);

								_DoItemAdded( folderItem, true);

								err = _SynchronizeWithFileSystem( folderItem);
							}
						}
					}

					for (VFileIterator fileIter( folder, FI_WANT_FILES | FI_WANT_INVISIBLES) ; fileIter.IsValid() && (err == VE_OK) ; ++fileIter )
					{
						VString fileName;
						fileIter->GetName( fileName);
						if (fileName == L".DS_Store")
							continue;

					#if SOLUTION_PROFILING_ENABLED
						if (	VSolutionProfiler::Get()->VFile_ConformsTo( fileIter, RIAFileKind::kProjectBackupFileKind)
							||	VSolutionProfiler::Get()->VFile_ConformsTo( fileIter, RIAFileKind::kUAGCacheFileKind)
							||	VSolutionProfiler::Get()->VFile_ConformsTo( fileIter, RIAFileKind::kProjectFileKind) )
					#else
						if (	fileIter->ConformsTo( RIAFileKind::kProjectBackupFileKind)
							||	fileIter->ConformsTo( RIAFileKind::kUAGCacheFileKind)
							||	fileIter->ConformsTo( RIAFileKind::kProjectFileKind) )
					#endif
						{
							continue;
						}
						
						// Check whether the child folder exists
						VString relativePath( fileName);
						VProjectItem *fileItem = inItem->FindChildByRelativePath( relativePath);
						if (fileItem == NULL)
						{
						#if SOLUTION_PROFILING_ENABLED
							if (VSolutionProfiler::Get()->VFile_ConformsTo( fileIter, RIAFileKind::kCatalogFileKind))
						#else
							if (fileIter->ConformsTo( RIAFileKind::kCatalogFileKind))
						#endif
							{
								VError lErr = VE_OK;
								VCatalog *catalog = VCatalog::Instantiate( lErr, fileName);
								if (lErr == VE_OK && catalog != NULL)
								{
									fileItem = catalog->GetCatalogItem();
									if (testAssert( fileItem != NULL))
									{
										inItem->AttachChild( fileItem);
									}
								}
								else
								{
									ReleaseRefCountable( &catalog);
								}
							}
							else
							{
								fileItem = inItem->NewChild( VProjectItem::eFILE);

							}

							if (fileItem != NULL)
							{
								fileItem->SetDisplayName( fileName);
								fileItem->SetName( fileName);
								fileItem->SetRelativePath(relativePath);

								_DoItemAdded( fileItem, true);
							}
						}
					}
				}
				ReleaseRefCountable( &folder);
			}
		}
	}

#if SOLUTION_PROFILING_ENABLED
	VSolutionProfiler::Get()->ExitCall( PROFILER_CALL_VProject__SynchronizeWithFileSystem);
#endif

	return err;
}


void VProject::_DoItemAdded( VProjectItem *inItem, bool inTouchProjectFile)
{
	// Append the item
	RegisterProjectItemInMapOfFullPathes( inItem);

	if (inItem->IsTagged() && !_IsItemReferenced( inItem))
		_ReferenceItem( inItem, inTouchProjectFile);

	// Append the children
	for (VProjectItemIterator iter(inItem) ; iter.IsValid() ; ++iter)
	{
		_DoItemAdded( iter, inTouchProjectFile);
	}

}


void VProject::_DoItemRemoved( VProjectItem *inItem, bool inTouchProjectFile)
{
	// Remove the children
	for (VProjectItemIterator iter(inItem) ; iter.IsValid() ; ++iter)
	{
		_DoItemRemoved( iter, inTouchProjectFile);
	}

	// Remove the item
	UnregisterProjectItemFromMapOfFullPathes( inItem);
	
	if (_IsItemReferenced( inItem))
		_UnreferenceItem( inItem, inTouchProjectFile);
}


VError VProject::_LoadProjectFile()
{
	VError err = VE_OK;

	if (fProjectItem != NULL && fProjectItemProjectFile != NULL)
	{
		fIsLoadingFromXML = true;

		VFilePath projectFilePath;
		fProjectItemProjectFile->GetFilePath( projectFilePath);
		VFile projectFile( projectFilePath);

		VValueBag bagProject;

		err = LoadBagFromXML( projectFile, fProjectItem->GetXMLElementName(), bagProject, XML_ValidateNever);
		if (err == VE_OK)
		{
			if (fSourceControlConnector)
			{
				VString sourceControlID;
				bagProject.GetString(ProjectItemBagKeys::sourceControlMode, sourceControlID);
				if (sourceControlID.IsEmpty() && fSolution)
					sourceControlID = fSolution->GetSourceControlID();
				fSourceControlConnector->SetSourceControlID(sourceControlID);
				if (!fSourceControlConnector->GetSourceControlID().IsEmpty())
				{
					VString sourceControlID = fSourceControlConnector->GetSourceControlID();
					fSourceControlConnector->ConnectToSourceControl(sourceControlID);
				}	
			}

			// avant tout, il faut construire l'arbre complet avant de pouvoir ajouter des proprietes aux items
			// ----------------------------------------
			// 1ere boucle : traiter l'ensemble des folders rencontres
			// ----------------------------------------
			std::vector<VProjectItemTag> singleFolderTagsFound;
			VBagArray* bagArrayFolders = bagProject.RetainElements(kXML_ELEMENT_FOLDER);
			for (sLONG i = 1; i <= bagArrayFolders->GetCount(); i++)
			{
				VValueBag *bagFolder = bagArrayFolders->GetNth(i);
				
				// recuperer le path relatif depuis le fichier projet
				VString strFolderRelativePath;
				bagFolder->GetString(ProjectItemBagKeys::path, strFolderRelativePath);
				if (testAssert(!strFolderRelativePath.IsEmpty()))
				{
					strFolderRelativePath.TrimeSpaces();
					if (strFolderRelativePath[strFolderRelativePath.GetLength()-1] != POSIX_FOLDER_SEPARATOR)
						strFolderRelativePath += POSIX_FOLDER_SEPARATOR;

					VFilePath folderPath;
					BuildPathFromRelativePath( folderPath, strFolderRelativePath, FPS_POSIX);
					
					if (testAssert(folderPath.IsValid()))
					{
						VProjectItem *projectItem = GetProjectItemFromFullPath( folderPath.GetPath());

						if (projectItem == NULL)
						{
							// sc 30/03/2012, even if the physical folder doesn't exist, the item must be referenced if needed

							// either the physical folder is outside the project folder or the physical folder doesn't exist
							VFilePath projectFolderPath;
							GetProjectFolderPath( projectFolderPath);
							if (folderPath.IsChildOf( projectFolderPath))
							{
								VError lError = VE_OK;
								projectItem = _CreateFolderItemFromPath( lError, folderPath, true, false, false);
								xbox_assert(lError == VE_OK);
							}
							else
							{
								// TBD: support external reference
								xbox_assert(false);
							}
						}

						if (projectItem != NULL)
						{
							// Read the item tags
							VBagArray *tagArray = bagFolder->RetainElements( kXML_ELEMENT_TAG);
							if (tagArray != NULL)
							{
								for (VIndex pos = 1 ; pos <= tagArray->GetCount() ; ++pos)
								{
									VProjectItemTag tagName = ProjectItemBagKeys::name.Get( tagArray->GetNth( pos));
									if (VProjectItemTagManager::Get()->IsRegisteredProjectItemTag( tagName))
									{
										// Sanity check
										if ((VProjectItemTagManager::Get()->GetProperties( tagName) & ePITP_ApplyToSingleFolder) != 0)
										{
											if (std::find( singleFolderTagsFound.begin(), singleFolderTagsFound.end(), tagName) == singleFolderTagsFound.end())
											{
												singleFolderTagsFound.push_back( tagName);
												projectItem->AddTag( tagName);
											}
										}
										else
										{
											projectItem->AddTag( tagName);
										}
									}
								}
							}
							ReleaseRefCountable( &tagArray);
							
							// Finally, reference the item
							if (projectItem->IsTagged() || projectItem->IsExternalReference())
							{
								_ReferenceItem( projectItem, false);
							}
						}
					}
				}
			}
			ReleaseRefCountable( &bagArrayFolders);

			// -----------------------------------------------------------
			// on traite ensuite l'ensemble des fichiers
			// -----------------------------------------------------------
			std::vector<VProjectItemTag> singleFileTagsFound;
			VBagArray* bagArrayFiles = bagProject.RetainElements(kXML_ELEMENT_FILE);
			for (sLONG i = 1; i <= bagArrayFiles->GetCount(); i++)
			{
				VValueBag* bagFile = bagArrayFiles->GetNth(i);
				// recuperer le path relatif depuis le fichier projet
				VString strFileRelativePath;
				bagFile->GetString(ProjectItemBagKeys::path, strFileRelativePath);
				if (testAssert(!strFileRelativePath.IsEmpty()))
				{
					// en deduire le path complet du fichier
					VFilePath filePath;
					strFileRelativePath.TrimeSpaces();
					BuildPathFromRelativePath( filePath, strFileRelativePath, FPS_POSIX);

					if (testAssert(filePath.IsValid()))
					{
						VProjectItem *projectItem = GetProjectItemFromFullPath( filePath.GetPath());

						if (projectItem == NULL)
						{
							// sc 30/03/2012, even if the physical file doesn't exist, the item must be referenced if needed

							// either the physical file is outside the project folder or the physical file doesn't exist
							VFilePath projectFolderPath;
							GetProjectFolderPath( projectFolderPath);
							if (filePath.IsChildOf( projectFolderPath))
							{
								VError lError = VE_OK;
								projectItem = _CreateFileItemFromPath( lError, filePath, true, false);
								xbox_assert(lError == VE_OK);
							}
							else
							{
								// TBD: support external reference
								xbox_assert(false);
							}
						}

						if (projectItem)
						{
							// Read the item tags
							VBagArray *tagArray = bagFile->RetainElements( kXML_ELEMENT_TAG);
							if (tagArray != NULL)
							{
								for (VIndex pos = 1 ; pos <= tagArray->GetCount() ; ++pos)
								{
									VProjectItemTag tagName = ProjectItemBagKeys::name.Get( tagArray->GetNth( pos));
									if (VProjectItemTagManager::Get()->IsRegisteredProjectItemTag( tagName))
									{
										// Sanity check
										if ((VProjectItemTagManager::Get()->GetProperties( tagName) & ePITP_ApplyToSingleFile) != 0)
										{
											if (std::find( singleFileTagsFound.begin(), singleFileTagsFound.end(), tagName) == singleFileTagsFound.end())
											{
												singleFileTagsFound.push_back( tagName);
												projectItem->AddTag( tagName);
											}
										}
										else
										{
											projectItem->AddTag( tagName);
										}
									}
								}
							}
							ReleaseRefCountable( &tagArray);
							
							// Finally, reference the item
							if (projectItem->IsTagged() || projectItem->IsExternalReference())
							{
								_ReferenceItem( projectItem, false);
							}
						}
					}
				}
			}


			ReleaseRefCountable( &bagArrayFiles);
		}
		else
		{
			err = VE_SOMA_PROJECT_FILE_COULD_NOT_OPENED;
			VErrorBase* errorBase = new VErrorBase(err, 0);
			errorBase->GetBag()->SetString(kSOMA_FILE_FULL_PATH, projectFilePath.GetPath());
			VTask::GetCurrent()->PushRetainedError(errorBase);
		}
		fIsLoadingFromXML = false;
	}


	return err;
}


VError VProject::_SaveProjectFile(bool inForceSave)
{
	VError err = VE_OK;

	if (inForceSave || (GetSolution()->IsAutoSave() && (fProjectFileDirtyStamp > 0)))
	{
		VProjectItem* projectFileProjectItem = GetProjectItemProjectFile();

		if (projectFileProjectItem != NULL)
		{
			// checkout du fichier 4DProject
			if (fSourceControlConnector && fSourceControlConnector->GetSourceControl())
			{
				VectorOfProjectItems projectFileProjectItems;
				projectFileProjectItems.push_back(projectFileProjectItem);
				err = CheckOut(projectFileProjectItems, true);
			}

			// TO DO 030409 if (err == VE_OK)
			{
				VFileDesc *fd = NULL;
				VFilePath projectFilePath;
				projectFileProjectItem->GetFilePath(projectFilePath);
				VFile projectFile(projectFilePath);

				if (projectFile.Exists())
				{
					EFileAttributes fileAttributes;
					projectFile.GetFileAttributes( fileAttributes);
					if ((fileAttributes & FATT_LockedFile) == 0)
					{
						// creation d'un fichier backup
						VFilePath backupProjectFilePath(projectFilePath);
						backupProjectFilePath.SetExtension(RIAFileKind::kBackupFileExtension);
						projectFile.CopyTo(backupProjectFilePath, NULL, FCP_Overwrite);
					}
				}

				err = projectFile.Open(FA_READ_WRITE, &fd, FO_CreateIfNotFound | FO_Overwrite);
				if (err == VE_OK && fd != NULL)
				{

					VString strXML;
					strXML.FromCString(INTRO_XML_UTF8);
					
					VValueBag* bagProject = new VValueBag();
					if (bagProject != NULL)
					{
						VString strProjectPath;
						fProjectItem->GetURL().GetPath(strProjectPath, eURL_POSIX_STYLE, false);

						// Save referenced items
						for (VectorOfProjectItems::iterator iter = fReferencedItems.begin() ; iter != fReferencedItems.end() && err == VE_OK ; ++iter)
						{
							VProjectItem *item = *iter;
							if (!item->IsGhost())
							{
								VValueBag *itemBag = new VValueBag();
								if (itemBag != NULL)
								{
									// Resolve the relative path;
									VFilePath projectItemFilePath;
									item->GetFilePath( projectItemFilePath);
									VURL projectItemURL( projectItemFilePath);
									
									VString strNetLoc, strProjectItemPath, strRelativeProjectItemPath;
									projectItemURL.GetNetworkLocation( strNetLoc, false);
									projectItemURL.GetPath( strProjectItemPath, eURL_POSIX_STYLE, false);
									if (!strNetLoc.IsEmpty())
										strProjectItemPath = "//" + strNetLoc + "/" + strProjectItemPath;
									GetSolution()->GetPathRelativeToFolderPath( strProjectPath, strProjectItemPath, strRelativeProjectItemPath);

									// Append the relative path
									ProjectItemBagKeys::path.Set( itemBag, strRelativeProjectItemPath);

									// Append the tags
									std::vector<VProjectItemTag> tags;
									item->GetTags( tags);
									if (!tags.empty())
									{
										VBagArray *tagArray = new VBagArray();
										if (tagArray != NULL)
										{
											for (std::vector<VProjectItemTag>::iterator iter = tags.begin() ; iter != tags.end() && err == VE_OK ; ++iter)
											{
												VValueBag *bag = new VValueBag();
												if (bag != NULL)
												{
													ProjectItemBagKeys::name.Set( bag, *iter);
													tagArray->AddTail( bag);
												}
												else
												{
													err = VE_MEMORY_FULL;
												}
												ReleaseRefCountable( &bag);
											}
											itemBag->SetElements( kXML_ELEMENT_TAG, tagArray);
										}
										else
										{
											err = VE_MEMORY_FULL;
										}
										ReleaseRefCountable( &tagArray);
									}

									if (err == VE_OK)
									{
										VString elementName = item->GetXMLElementName();
										if (testAssert( elementName.GetLength() > 0))
										{
											bagProject->AddElement( elementName, itemBag);
										}
									}
								}
								else
								{
									err = VE_MEMORY_FULL;
								}
								ReleaseRefCountable( &itemBag);
							}
						}

						bagProject->DumpXML( strXML, fProjectItem->GetXMLElementName(), false);
					}
					else
					{
						err = VE_MEMORY_FULL;
					}
					ReleaseRefCountable( &bagProject);

					if (err == VE_OK)
					{
						VStringConvertBuffer buffer( strXML, VTC_UTF_8);
						err = fd->SetSize( buffer.GetSize());
						if (err == VE_OK)
						{
							err = fd->PutDataAtPos( buffer.GetCPointer(), buffer.GetSize());
							if (err == VE_OK)
							{
								err = fd->Flush();
							}
						}
					}
					delete fd;
				}
				else
				{
					err = VE_FILE_CANNOT_OPEN;
				}
			}
		}
	}

	if (err != VE_OK)
	{
		if (GetSolution()->GetSolutionMessageManager())
		{
			VString localizedMessage;
			GetSolution()->GetSolutionMessageManager()->GetLocalizedStringFromResourceName(kSOMA_PROJECT_FILE_COULD_NOT_BE_SAVED, localizedMessage);
			localizedMessage += ":\n";
			VFilePath projectVFilePath;
			GetProjectItemProjectFile()->GetFilePath(projectVFilePath);
			VString projectFullPath;
			projectVFilePath.GetPath(projectFullPath);
			localizedMessage += projectFullPath;
			GetSolution()->GetSolutionMessageManager()->DisplayMessage(localizedMessage);
		}
	}
	else
	{
		fProjectFileDirtyStamp = 0;
	}

	return err;
}


void VProject::_TouchProjectFile()
{
	++fProjectFileDirtyStamp;
}


void VProject::_UnreferenceItem( VProjectItem *inItem, bool inTouchProjectFile)
{
	VectorOfProjectItemsIterator found = std::find(  fReferencedItems.begin(), fReferencedItems.end(), inItem);
	if (found != fReferencedItems.end())
	{
		fReferencedItems.erase( found);
		if (inTouchProjectFile)
			_TouchProjectFile();
	}
}


void VProject::_ReferenceItem( VProjectItem *inItem, bool inTouchProjectFile)
{
	if (!_IsItemReferenced( inItem))
	{
		fReferencedItems.push_back( inItem);
		if (inTouchProjectFile)
			_TouchProjectFile();
	}
}


bool VProject::_IsItemReferenced( VProjectItem *inItem) const
{
	return (std::find( fReferencedItems.begin(), fReferencedItems.end(), inItem) != fReferencedItems.end());
}


bool VProject::_IsVectorContainsReferencedItems( const VectorOfProjectItems& inProjectItems, bool inRecursive) const
{
	bool result = false;

	for (VectorOfProjectItemsConstIterator iter = inProjectItems.begin() ; (iter != inProjectItems.end()) && !result ; ++iter)
	{
		result = _IsItemReferenced( *iter);
		if (!result && inRecursive)
		{
			VectorOfProjectItems children;
			VProjectItemTools::GetChildren( *iter, children, false);
			result = _IsVectorContainsReferencedItems( children, true);
		}
	}

	return result;
}


VProjectItem* VProject::_CreateFileItemFromPath( XBOX::VError& outError, const XBOX::VFilePath& inPath, bool inRecursive, bool inTouchProjectFile)
{
	VProjectItem *result = NULL;

	if (inPath.IsFile())
	{
		VFilePath parentPath, projectFolderPath;
		if (inPath.GetParent( parentPath) && GetProjectFolderPath( projectFolderPath))
		{
			if ((projectFolderPath == parentPath) || parentPath.IsChildOf( projectFolderPath))
			{
				VProjectItem *parentItem = GetProjectItemFromFullPath( parentPath.GetPath());
				if ((parentItem == NULL) && inRecursive)
				{
					parentItem = _CreateFolderItemFromPath( outError, parentPath, true, false, inTouchProjectFile);
				}

				if (parentItem != NULL)
				{
					VFile file(inPath);
					VString fileName;
					inPath.GetFileName( fileName);

				#if SOLUTION_PROFILING_ENABLED
					if (VSolutionProfiler::Get()->VFile_ConformsTo( &file, RIAFileKind::kCatalogFileKind))
				#else
					if (file.ConformsTo( RIAFileKind::kCatalogFileKind))
				#endif
					{
						VError lErr = VE_OK;
						VCatalog *catalog = VCatalog::Instantiate( lErr, fileName);
						if (lErr == VE_OK && catalog != NULL)
						{
							result = catalog->GetCatalogItem();
							if (testAssert( result != NULL))
							{
								parentItem->AttachChild( result);
							}
						}
						else
						{
							ReleaseRefCountable( &catalog);
						}
					}
					else
					{
						result = parentItem->NewChild( VProjectItem::eFILE);
					}

					if (result != NULL)
					{

						result->SetName( fileName);
						result->SetDisplayName( fileName);
						result->SetRelativePath( fileName);

						_DoItemAdded( result, inTouchProjectFile);

						if (!result->ContentExists())
							result->SetPhysicalLinkValid( false);
					}
				}
			}
		}
	}
	return result;
}


VProjectItem* VProject::_CreateFolderItemFromPath( XBOX::VError& outError, const XBOX::VFilePath& inPath, bool inRecursive, bool inSynchronizeWithFileSystem, bool inTouchProjectFile)
{
	VProjectItem *result = NULL;

	if (inPath.IsFolder())
	{
		VFilePath parentPath, projectFolderPath;
		if (inPath.GetParent( parentPath) && GetProjectFolderPath( projectFolderPath))
		{
			if ((projectFolderPath == parentPath) || parentPath.IsChildOf( projectFolderPath))
			{
				VProjectItem *parentItem = GetProjectItemFromFullPath( parentPath.GetPath());
				if ((parentItem == NULL) && inRecursive)
				{
					parentItem = _CreateFolderItemFromPath( outError, parentPath, true, false, inTouchProjectFile);
				}

				if (parentItem != NULL)
				{
					result = parentItem->NewChild( VProjectItem::eFOLDER);
					if (result != NULL)
					{
						VString itemName;
						inPath.GetFolderName( itemName);
						result->SetName( itemName);
						result->SetDisplayName( itemName);
						itemName += FOLDER_SEPARATOR;
						result->SetRelativePath( itemName);

						_DoItemAdded( result, inTouchProjectFile);

						if (!result->ContentExists())
							result->SetPhysicalLinkValid( false);
					}
				}
			}
		}
	}
	return result;
}


VProjectItem *VProject::_GetEntityModelProjectItem( VProjectItem *inCurItemToSearch )
{
	VProjectItem *ret = NULL;
	if (!inCurItemToSearch)	inCurItemToSearch = fProjectItem;

	for (VProjectItemIterator it( inCurItemToSearch ); !ret && it.IsValid(); ++it)
	{
		if (it->ConformsTo( RIAFileKind::kCatalogFileKind))	ret = it;
		if (!ret)	ret = _GetEntityModelProjectItem( it );
	}

	return ret;
}


void VProject::_GetProjectItemsByExtension( const XBOX::VString &inExtension, VectorOfProjectItems &outItems )
{
	_GetProjectItemsByExtension( fProjectItem, inExtension, outItems );
}

void VProject::_GetProjectItemsByExtension( VProjectItem *inProjectItem, const XBOX::VString &inExtension, VectorOfProjectItems &outItems )
{
	VProjectItem* projectItem = NULL;

	for (VProjectItemIterator it(inProjectItem); it.IsValid(); ++it)
	{
		_GetProjectItemsByExtension(it, inExtension, outItems);
	}

	if (projectItem == NULL)
	{
		VString extension;
		inProjectItem->GetExtension(extension);

		if (extension == inExtension)
		{
			outItems.push_back(inProjectItem);
		}
	}
}


// ----------------------------------------------------------------------------
// Classe VCatalog :
// ----------------------------------------------------------------------------


VCatalog::VCatalog()
: fCatalogItem(NULL)
,fCatalogBag(NULL)
{
}

VCatalog::~VCatalog()
{
	ReleaseRefCountable( &fCatalogBag);
}


VCatalog* VCatalog::Instantiate( VError& outError, const XBOX::VString& inCatalogFileName)
{
	VCatalog *catalog = NULL;
	
	outError = VE_OK;
	
	// Create the root  item of the catalog
	VProjectItem *rootItem = new VProjectItem( VProjectItem::eCATALOG_FILE);
	if (rootItem != NULL)
	{
		rootItem->SetName( inCatalogFileName);
		rootItem->SetDisplayName( inCatalogFileName);

		catalog = new VCatalog();
		if (catalog != NULL)
		{
			catalog->_SetCatalogItem( rootItem);
		}
		else
		{
			outError = VE_MEMORY_FULL;
		}
		ReleaseRefCountable( &rootItem);
	}
	else
	{
		outError = VE_MEMORY_FULL;
	}
	return catalog;
}


void VCatalog::GetCatalogPath( XBOX::VFilePath &outPath ) const
{
	if (fCatalogItem)
	{
		fCatalogItem->GetFilePath( outPath );
	}
}

VError VCatalog::GetCatalogContent(XBOX::VString& outCatalogContent)
{
	VError err = VE_UNKNOWN_ERROR;

	const VValueBag * bag = RetainCatalogBag();
	if (bag != NULL)
		err =  bag->GetJSONString(outCatalogContent);
	else
		outCatalogContent.Clear();
	XBOX::ReleaseRefCountable( &bag);

	return err;
}

const VValueBag *VCatalog::RetainCatalogBag() const
{
	return RetainRefCountable( fCatalogBag);
}


void VCatalog::_SetCatalogItem( VProjectItem *inProjectItem)
{
	if (testAssert(fCatalogItem == NULL && inProjectItem != NULL))
	{
		fCatalogItem = RetainRefCountable( inProjectItem);

		VProjectItemCatalog *behaviour = dynamic_cast<VProjectItemCatalog*>(fCatalogItem->GetBehaviour());
		if (testAssert(behaviour != NULL))
			behaviour->SetCatalog( this);
	}
}


/*
	static
*/
VError VCatalog::_ParseCatalogAndCreateProjectItems( CDB4DBase* inDB4DBase, VProjectItem *inProjectItem, VValueBag *outCatalogBag)
{
	VErrorDB4D err = VE_OK;

	if (testAssert( outCatalogBag != NULL))
	{
		std::vector<XBOX::VRefPtr<CDB4DEntityModel> > entityModels;
		CDB4DBaseContext* dB4DBaseContext = inDB4DBase->NewContext(nil, nil);
		err = inDB4DBase->RetainAllEntityModels(entityModels, dB4DBaseContext, true);
		if (err == VE_OK)
		{
			VProjectItem* newProjectItemEntityModel = NULL;
			for( std::vector<XBOX::VRefPtr<CDB4DEntityModel> >::iterator oneEntityModel = entityModels.begin() ; oneEntityModel != entityModels.end() ; ++oneEntityModel)
			{			
				// pour creer les VProjectItems ...	
				newProjectItemEntityModel = inProjectItem->NewChild(VURL(), VProjectItem::eDATA_CLASS);
				newProjectItemEntityModel->SetDisplayName((*oneEntityModel)->GetEntityName());
				newProjectItemEntityModel->SetName((*oneEntityModel)->GetEntityName());

				// ... et leurs attributs
				sLONG entityModelAttributesCount = (*oneEntityModel)->CountAttributes();
				for (sLONG j = 1; j <= entityModelAttributesCount; ++j)
				{
					CDB4DEntityAttribute* entityAttribute = (*oneEntityModel)->GetAttribute(j);
					if (entityAttribute)
					{							
						VProjectItem* newProjectItemEntityModelAttribute = newProjectItemEntityModel->NewChild(VURL(), VProjectItem::eDATA_CLASS_ATTRIBUTE);
						VString entityAttributeName;
						entityAttribute->GetAttibuteName(entityAttributeName);
						newProjectItemEntityModelAttribute->SetDisplayName(entityAttributeName);
						newProjectItemEntityModelAttribute->SetName(entityAttributeName);
					}
				}

				// et aussi pour renseigner le fCatalogBag
				VValueBag* resolvedRepresentation = (*oneEntityModel)->CreateDefinition();
				outCatalogBag->AddElement(d4::dataClasses, resolvedRepresentation);
				ReleaseRefCountable( &resolvedRepresentation);
			}
		}
		XBOX::ReleaseRefCountable(&dB4DBaseContext);
	}

	return err;
}


VError VCatalog::ParseCatalogAndCreateProjectItems(CDB4DBase* inDB4DBase)
{
	VErrorDB4D err = VE_OK;

	if (fCatalogItem != NULL)
	{
		VValueBag *newCatalogBag = new VValueBag;
		if (inDB4DBase == NULL)
		{
			VFilePath catalogFilePath;
			fCatalogItem->GetFilePath(catalogFilePath);
			VFile catalogFile(catalogFilePath);
			CDB4DManager *dB4DBaseManager = VComponentManager::RetainComponentOfType<CDB4DManager>();
			sLONG flags = DB4D_Open_As_XML_Definition | DB4D_Open_No_Respart | DB4D_Open_StructOnly | DB4D_Open_Allow_Temporary_Convert_For_ReadOnly;
			CDB4DBase *dB4DBase = dB4DBaseManager->OpenBase(catalogFile, flags, NULL /* outErr */, XBOX::FA_READ);

			if (dB4DBase != NULL)
			{
				err = _ParseCatalogAndCreateProjectItems( dB4DBase, fCatalogItem, newCatalogBag);

				dB4DBase->CloseAndRelease(false);
				dB4DBaseManager->Release();
			}
		}
		else
		{
			err = _ParseCatalogAndCreateProjectItems( inDB4DBase, fCatalogItem, newCatalogBag);
		}

		if (err == VE_OK)
			CopyRefCountable( &fCatalogBag, newCatalogBag);

		ReleaseRefCountable( &newCatalogBag);
	}

	return err;
}

void VCatalog::GetEntityModelNames(VectorOfVString& outEntityNames) const
{
	const VValueBag *catalogBag = RetainCatalogBag();
	if (catalogBag != NULL)
	{
		const VBagArray* bagModels = catalogBag->GetElements( d4::dataClasses);
		VIndex count = (bagModels != NULL) ? bagModels->GetCount() : 0;
		for( VIndex i = 1 ; i <= count ; ++i)
		{
			const VValueBag* bagEntityModel = bagModels->GetNth(i);
			if (bagEntityModel)
			{
				VString name;
				bagEntityModel->GetString( d4::name, name);
				outEntityNames.push_back( name);
			}
		}
	}
	ReleaseRefCountable( &catalogBag);
}

void VCatalog::GetEntityModelAttributes(const VString& inEntityName, std::vector<IEntityModelCatalogAttribute* >& outAttributes) const
{
	const VValueBag *catalogBag = RetainCatalogBag();
	if (catalogBag != NULL)
	{
		const VBagArray* bagModels = catalogBag->GetElements( d4::dataClasses);
		VIndex count = (bagModels != NULL) ? bagModels->GetCount() : 0;
		for( VIndex i = 1 ; i <= count ; ++i)
		{
			const VValueBag* bagEntityModel = bagModels->GetNth(i);
			if (bagEntityModel)
			{
				VString entityName;
				bagEntityModel->GetString(d4::name, entityName);
				if (entityName == inEntityName)
				{
					const VBagArray* bagEntityModelAttributes = bagEntityModel->GetElements(d4::attributes);
					if (bagEntityModelAttributes)
					{
						for (sLONG j = 1; j <= bagEntityModelAttributes->GetCount(); ++j)
						{
							VString name, type;

							bagEntityModelAttributes->GetNth(j)->GetString( d4::name, name);
							bagEntityModelAttributes->GetNth(j)->GetString( d4::type, type);
							
							outAttributes.push_back( new VCatalogAttribute(name, type) );
						}
					}
					break;
				}
			}
		}
	}
	ReleaseRefCountable( &catalogBag);
}


void VCatalog::GetEntityModelMethods(const VString& inEntityName, std::vector< IEntityModelCatalogMethod* >& outMethods) const
{
	const VValueBag *catalogBag = RetainCatalogBag();
	if (catalogBag != NULL)
	{
		const VBagArray* bagModels = catalogBag->GetElements( d4::dataClasses);
		VIndex count = (bagModels != NULL) ? bagModels->GetCount() : 0;
		for( VIndex i = 1 ; i <= count ; ++i)
		{
			const VValueBag* bagEntityModel = bagModels->GetNth(i);
			if (bagEntityModel)
			{
				VString entityName;
				bagEntityModel->GetString(d4::name, entityName);
				if (entityName == inEntityName)
				{
					const VBagArray* bag = bagEntityModel->GetElements(d4::methods);
					if (bag)
					{
						for (sLONG j = 1; j <= bag->GetCount(); ++j)
						{
							VString name, applyTo;

							bag->GetNth(j)->GetString( d4::name, name);
							bag->GetNth(j)->GetString( d4::applyTo, applyTo);

							outMethods.push_back( new VCatalogMethod(name, applyTo) );
						}
					}
					break;
				}
			}
		}
	}
	ReleaseRefCountable( &catalogBag);
}



// ----------------------------------------------------------------------------
// Classe VProjectUser :
// ----------------------------------------------------------------------------
VProjectUser::VProjectUser(VProject* inProject)
{
	fProject = inProject;
}

VProjectUser::~VProjectUser()
{
}

VError VProjectUser::LoadFromUserFile(VFile& inUserFile)
{
	VError err = VE_OK;

	if (fProject && inUserFile.Exists() )
	{
		VFileDesc *fd = NULL;
		err = inUserFile.Open(FA_READ, &fd, FO_Default);
		if (err != VE_OK)
			VTask::FlushErrors();	// ce n'est pas grave s'il y a eu une erreur (fichier absent pex)
		if (err == VE_OK && fd != NULL)
		{
			VPtr ptr = VMemory::NewPtr(fd->GetSize(), kSOLUTION_MANAGER_SIGNATURE);
			if (ptr != NULL)
			{
				err = fd->GetDataAtPos(ptr, fd->GetSize());
				if (err == VE_OK)
				{
					VString xmlString(ptr, fd->GetSize(), VTC_UTF_8);
					/*VValueBag* bagSourceControl = ::CreateBagFromXMLFile(&inUserFile, kXML_ELEMENT_SOURCE_CONTROL, XML_ValidateNever, false);
					
					if (bagSourceControl != NULL)
					{

						bagSourceControl->Release();
					}
					*/
				}
				VMemory::DisposePtr(ptr);
			}
		}
		else
		{
			//err = VE_SOMA_SOLUTION_LINK_FILE_COULD_NOT_OPENED;
		}
		delete fd;
	}

	return err;
}

VError VProjectUser::SaveToUserFile(VFile& inUserFile) const
{
	VError err = VE_OK;

	if (fProject)
	{
		VFileDesc *fd = NULL;
		err = inUserFile.Open(FA_READ_WRITE, &fd, FO_CreateIfNotFound | FO_Overwrite);
		if (err == VE_OK && fd != NULL)
		{
			VString strXML;
			strXML.FromCString(INTRO_XML_UTF8);

			VValueBag* bagSourceControl = new VValueBag();
			ProjectItemBagKeys::sourceControlId.Set(bagSourceControl, fProject->GetSourceControlConnector()->GetSourceControlID());
			VString sourceControlID = fProject->GetSourceControlConnector()->GetSourceControlID();
			if (sourceControlID.IsEmpty())
				ProjectItemBagKeys::sourceControlMode.Set(bagSourceControl, fProject->GetSolution()->GetSourceControlID());
			bagSourceControl->DumpXML(strXML, kXML_ELEMENT_SOURCE_CONTROL, false);
			bagSourceControl->Release();

			VStringConvertBuffer buffer(strXML, VTC_UTF_8);
			err = fd->SetSize(buffer.GetSize());
			if (err == VE_OK)
			{
				err = fd->PutDataAtPos(buffer.GetCPointer(), buffer.GetSize());
				if (err == VE_OK)
					err = fd->Flush();
			}
			delete fd;
		}
		else
		{
			err = VE_FILE_CANNOT_OPEN;
		}
	}

	return err;
}