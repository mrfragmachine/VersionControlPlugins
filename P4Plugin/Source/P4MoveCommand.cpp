#include "FileSystem.h"
#include "P4Command.h"
#include "P4Task.h"
#include "P4Utility.h"
#include "msgserver.h"

using namespace std;

class P4MoveCommand : public P4Command
{
public:
	P4MoveCommand(const char* name) : P4Command(name), m_bMoveDisabledOnServer(false) {}

	virtual bool Run(P4Task& task, const CommandArgs& args)
	{
		char msgBuf[1024];

		ClearStatus();
		Conn().Log().Info() << args[0] << "::Run()" << Endl;
		
		VersionedAssetList incomingAssetList;
		Conn() >> incomingAssetList;

		if ( incomingAssetList.empty() ) 
		{
			Conn().EndResponse();
			return true;
		}
		
		// Get the latest state for the asset.
		VersionedAssetList assetList;
		RunAndGetStatus(task, incomingAssetList, assetList);

		// Process two assets at a time ie. src,dest
		if ( assetList.size() % 2 ) 
		{
			Conn().WarnLine("uneven number of assets during move", MASystem);
			Conn().EndResponse();
			return true;
		}

		if (m_bMoveDisabledOnServer)
		{
			MoveUsingIntegrateAddDelete(task, args, assetList);
		}
		else
		{
			if (!MoveUsingMoveCommand(task, args, assetList))
			{
				if (m_bMoveDisabledOnServer)
				{
					RunAndGetStatus(task, assetList, assetList);
					MoveUsingIntegrateAddDelete(task, args, assetList);
				}
			}
		}

		VersionedAssetList::const_iterator b = assetList.begin();
		VersionedAssetList::const_iterator e = b;
		
		VersionedAssetList targetAssetList;
		while (!HasErrors() && b != assetList.end())
		{
			e += 2;
			
			const VersionedAsset& src = *b;
			const VersionedAsset& dest = *(b+1);

			targetAssetList.push_back(dest);
			
			// Make the actual file system move if perforce didn't do it ie. in
			// the case of an empty folder rename or a non versioned asset/folder move/rename
			if (!PathExists(dest.GetPath()))
			{
				// Move the file
				if (!MoveAFile(src.GetPath(), dest.GetPath()))
				{
					_snprintf_s(msgBuf, _TRUNCATE, "Error moving file %s to %s.\n", src.GetPath().c_str(),
						dest.GetPath().c_str());
					Conn().WarnLine(msgBuf);
				}
			}

			// Delete move folder src since perforce leaves around empty folders.
			// This only works because unity will not send embedded moves.
			if (src.IsFolder() && IsDirectory(src.GetPath()))
			{
				DeleteRecursive(src.GetPath());
			}

			b = e;
		}
		
		// We just wrap up the communication here.
		Conn() << GetStatus();
		
		RunAndSendStatus(task, targetAssetList);
		
		Conn().EndResponse();

		return true;
	}

private:

	bool MoveUsingMoveCommand(P4Task& task, const CommandArgs& args, const VersionedAssetList& assetList)
	{
		bool noLocalFileMove = args.size() > 1 && args[1] == "noLocalFileMove";

		VersionedAssetList::const_iterator b = assetList.begin();
		VersionedAssetList::const_iterator e = b;

		// Split into two steps. 1st make everything editable and 2nd do the move.
		// this makes changes more atomic.
		string editPaths;

		while (b != assetList.end())
		{
			e += 2;
			const VersionedAsset& src = *b;
			bool editable = (src.GetState() & (kCheckedOutLocal | kAddedLocal | kLockedLocal)) != 0;

			if (editable)
			{
				Conn().Log().Debug() << "Already editable source " << src.GetPath() << Endl;
			}
			else
			{
				editPaths += " ";
				editPaths += ResolvePaths(b, b+1, kPathWild | kPathRecursive);
			}
			b = e;
		}

		if (!editPaths.empty())
		{
			task.CommandRun("edit " + editPaths, this);
		}

		b = assetList.begin();
		e = b;

		string noLocalFileMoveFlag = noLocalFileMove ? " -k " : "";
		while (!HasErrors() && b != assetList.end())
		{
			e += 2;

			const VersionedAsset& src = *b;
			const VersionedAsset& dest = *(b+1);

			string paths = ResolvePaths(b, e, kPathWild | kPathRecursive);

			if (!task.CommandRun("move " + noLocalFileMoveFlag + paths, this) || m_bMoveDisabledOnServer)
			{
				return false;
			}

			b = e;
		}

		return true;
	}

	bool MoveUsingIntegrateAddDelete(P4Task& task, const CommandArgs& args, const VersionedAssetList& assetList)
	{
		char msgBuf[1024];

		VersionedAssetList::const_iterator b = assetList.begin();
		VersionedAssetList::const_iterator e = b;

		while (!HasErrors() && b != assetList.end())
		{
			e += 2;

			const VersionedAsset& src = *b;
			const VersionedAsset& dest = *(b+1);

			string resolvedSrcPath = ResolvePaths(b, b+1, kPathWild | kPathRecursive);
			string resolvedDstPath = ResolvePaths(b+1, b+2, kPathWild | kPathRecursive);
			string resolvedSrcDstPaths = ResolvePaths(b, e, kPathWild | kPathRecursive);

			// An added file can't be integrated. It must instead be moved locally on disk, 
			// reverted and then re-added to the changelist. 
			if (src.GetState() & kAddedLocal)
			{
				_snprintf_s(msgBuf, _TRUNCATE, "%s added locally.\n", src.GetPath().c_str());
				Conn().InfoLine(msgBuf);

				if (MoveAFile(src.GetPath(), dest.GetPath()))
				{
					if (!task.CommandRun("add " + resolvedDstPath, this))
					{
						break;
					}

					if (!task.CommandRun("revert " + resolvedSrcPath, this))
					{
						break;
					}
				}
				else
				{
					_snprintf_s(msgBuf, _TRUNCATE, "Error moving file %s to %s.\n", src.GetPath().c_str(),
						dest.GetPath().c_str());
					Conn().WarnLine(msgBuf);
				}
			}
			// A file that's checked out can have been modified. In order to preserve the content of the file it's
			// first integrated and then moved locally on disk to its new destination. After that the original file
			// can be reverted and deleted from the depot.
			else if (src.GetState() & kCheckedOutLocal)
			{
				_snprintf_s(msgBuf, _TRUNCATE, "%s checked out locally.\n", src.GetPath().c_str());
				Conn().InfoLine(msgBuf);

				// The '-Di' argument is needed to allow files affected by the Perforce 'move' command to be integrated.
				if (!task.CommandRun("integrate -Di " + resolvedSrcDstPaths, this))
				{
					break;
				}

				if (MoveAFile(src.GetPath(), dest.GetPath()))
				{
					if (!task.CommandRun("revert " + resolvedSrcPath, this))
					{
						break;
					}

					if (!task.CommandRun("delete " + resolvedSrcPath, this))
					{
						break;
					}
				}
				else
				{
					_snprintf_s(msgBuf, _TRUNCATE, "Error moving file %s to %s.\n", src.GetPath().c_str(),
						dest.GetPath().c_str());
					Conn().WarnLine(msgBuf);
				}
			}
			// The file has not been edited locally and can be safely integrated before 
			// deleting the source from the depot.
			else
			{
				_snprintf_s(msgBuf, _TRUNCATE, "%s not opened locally.\n", src.GetPath().c_str());
				Conn().InfoLine(msgBuf);

				// The '-Di' argument is needed to allow files affected by the Perforce 'move' command to be integrated.
				if (!task.CommandRun("integrate -Di " + resolvedSrcDstPaths, this))
				{
					break;
				}

				if (!task.CommandRun("delete " + resolvedSrcPath, this))
				{
					break;
				}
			}

			b = e;
		}

		return true;
	}

	// Default handler of P4 error output. Called by the default P4Command::Message() handler.
	void HandleError( Error *err )
	{
		ErrorId* errorId = err->GetId(0);
		const int uniqueCode = errorId ? errorId->UniqueCode() : 0;

		// The 'move' command will always fail if it's been disabled in the server.
		// Set the flag to fall back to using the integrate/add/delete commands.
		if( uniqueCode == MsgServer::MoveRejected.UniqueCode() )
		{
			m_bMoveDisabledOnServer = true;
		}
		else
		{
			P4Command::HandleError(err);
		}
	}

private:

	bool m_bMoveDisabledOnServer;

} cMove("move");

