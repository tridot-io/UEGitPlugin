// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlUtils.h"
#include "SourceControlHelpers.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#if ENGINE_MAJOR_VERSION >= 5
#include "HAL/PlatformFileManager.h"
#else
#include "HAL/PlatformFilemanager.h"
#endif

#include <thread>

#define LOCTEXT_NAMESPACE "GitSourceControl"

FName FGitConnectWorker::GetName() const
{
	return "Connect";
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// The connect worker checks if we are connected to the remote server.
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	// Skip login operations, since Git does not have to login.
	// It's not a big deal for async commands though, so let those go through.
	// More information: this is a heuristic for cases where UE is trying to create
	// a valid Perforce connection as a side effect for the connect worker. For Git,
	// the connect worker has no side effects. It is simply a query to retrieve information
	// to be displayed to the user, like in the revision control settings or on init.
	// Therefore, there is no need for synchronously establishing a connection if not there.
	if (InCommand.Concurrency == EConcurrency::Synchronous)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	// Check Git availability
	// We already know that Git is available if PathToGitBinary is not empty, since it is validated then.
	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText& NotFound = LOCTEXT("GitNotFound", "Failed to enable Git revision control. You need to install Git and ensure the plugin has a valid path to the git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	// Get default branch: git remote show
	
	TArray<FString> Parameters {
		TEXT("-h"), // Only limit to branches
		TEXT("-q") // Skip printing out remote URL, we don't use it
	};
	
	// Check if remote matches our refs.
	// Could be useful in the future, but all we want to know right now is if connection is up.
	// Parameters.Add("--exit-code");
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("ls-remote"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		const FText& NotFound = LOCTEXT("GitRemoteFailed", "Failed Git remote connection. Ensure your repo is initialized, and check your connection to the Git host.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
	}

	// TODO: always return true, and enter an offline mode if could not connect to remote
	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return false;
}

FName FGitCheckOutWorker::GetName() const
{
	return "CheckOut";
}

// [TriDot patch] 체크아웃 직전 원격 확인.
// 원본은 30초 주기 백그라운드 fetch로 갱신된 캐시 상태만 보고 잠그므로, 에디터를 켠 직후나
// 남이 제출한 직후에는 옛 판 파일에 잠금이 걸린다. 여기서 직접 fetch한 뒤 HEAD..@{u} 사이에
// 대상 파일 변경이 있으면 잠그지 않는다.
// - 저장소 기준: 서브모듈이면 Command가 바꿔 둔 PathToRepositoryRoot 하나로 조회·fetch·비교·상대 경로를 맞춘다.
// - upstream: Provider 캐시가 아니라 명령 시점에 그 저장소에서 다시 조회한다 (에디터 실행 중 브랜치 전환 대비).
// - 경로: git 출력을 상태 키로 쓰지 않고 요청 파일과 (대소문자 무시로) 대조해 요청 파일만 NotAtHead로 표시한다.
// - upstream 설정이 없을 때만 검사를 건너뛰고, 설정이 있는데 확인할 수 없으면 거부한다.
static bool EnsureFilesAtRemoteHead(FGitSourceControlCommand& InCommand, TMap<const FString, FGitState>& OutStates)
{
	const FString& Root = InCommand.PathToRepositoryRoot;
	TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, Root);
	RelativeFiles = RelativeFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);
	if (RelativeFiles.Num() == 0)
	{
		return true;
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	auto Refuse = [&InCommand, &Errors](const FString& Message)
	{
		InCommand.ResultInfo.ErrorMessages.Add(Message);
		InCommand.ResultInfo.ErrorMessages.Append(Errors);
		return false;
	};

	// upstream "설정 없음"(로컬 전용 브랜치, detached HEAD)만 검사를 건너뛴다.
	// 설정이 있는데 해석이 안 되는 경우(원격 추적 ref 없음 등)는 fetch 뒤 다시 해석하고, 그래도 실패하면 거부한다.
	const TArray<FString> BranchParams{ TEXT("--abbrev-ref"), TEXT("HEAD") };
	if (!GitSourceControlUtils::RunCommand(TEXT("rev-parse"), InCommand.PathToGitBinary, Root, BranchParams,
		FGitSourceControlModule::GetEmptyStringArray(), Results, Errors) || Results.Num() == 0)
	{
		return Refuse(TEXT("Check out refused: could not read the current branch."));
	}
	const FString Branch = Results[0].TrimStartAndEnd();
	Results.Reset();
	Errors.Reset();
	const TArray<FString> MergeParams{ TEXT("--get"), FString::Printf(TEXT("branch.%s.merge"), *Branch) };
	const bool bHasUpstreamConfig = Branch != TEXT("HEAD")
		&& GitSourceControlUtils::RunCommand(TEXT("config"), InCommand.PathToGitBinary, Root, MergeParams,
			FGitSourceControlModule::GetEmptyStringArray(), Results, Errors)
		&& Results.Num() > 0 && !Results[0].TrimStartAndEnd().IsEmpty();
	if (!bHasUpstreamConfig)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Check out: branch '%s' in '%s' has no upstream, skipping latest-revision check."), *Branch, *Root);
		return true;
	}

	// 인수 없는 fetch는 현재 브랜치에 설정된 remote(=@{u}의 remote)를 받는다.
	Results.Reset();
	Errors.Reset();
	const TArray<FString> FetchParams{ TEXT("--no-tags"), TEXT("--prune") };
	if (!GitSourceControlUtils::RunCommand(TEXT("fetch"), InCommand.PathToGitBinary, Root, FetchParams,
		FGitSourceControlModule::GetEmptyStringArray(), Results, Errors))
	{
		return Refuse(FString::Printf(TEXT("Check out refused: could not fetch the upstream of '%s' to verify the files are at the latest revision."), *Branch));
	}

	Results.Reset();
	Errors.Reset();
	const TArray<FString> UpstreamParams{ TEXT("--abbrev-ref"), TEXT("--symbolic-full-name"), TEXT("@{u}") };
	if (!GitSourceControlUtils::RunCommand(TEXT("rev-parse"), InCommand.PathToGitBinary, Root, UpstreamParams,
		FGitSourceControlModule::GetEmptyStringArray(), Results, Errors) || Results.Num() == 0 || Results[0].TrimStartAndEnd().IsEmpty())
	{
		return Refuse(FString::Printf(TEXT("Check out refused: upstream of '%s' is configured but could not be resolved after fetch."), *Branch));
	}
	const FString Upstream = Results[0].TrimStartAndEnd();

	// 경로 필터 없이 아직 받지 않은 커밋의 변경 파일 전체를 받아 요청 파일과 대조한다.
	// (pathspec은 대소문자를 구분해 Windows에서 casing이 다른 요청을 놓친다. FString 비교·TSet은 대소문자 무시.)
	// --no-renames: 원격에서 이름이 바뀐 파일도 옛 경로로 나온다. 비ASCII 경로는 RunCommand가 core.quotePath=false로 받는다.
	Results.Reset();
	Errors.Reset();
	const TArray<FString> LogParams{ TEXT("--no-renames"), TEXT("--pretty="), TEXT("--name-only"), TEXT("HEAD..@{u}") };
	if (!GitSourceControlUtils::RunCommand(TEXT("log"), InCommand.PathToGitBinary, Root, LogParams,
		FGitSourceControlModule::GetEmptyStringArray(), Results, Errors))
	{
		return Refuse(FString::Printf(TEXT("Check out refused: could not compare files with %s."), *Upstream));
	}

	TSet<FString> Changed;
	for (const FString& Line : Results)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		FPaths::NormalizeFilename(Trimmed);
		if (!Trimmed.IsEmpty())
		{
			Changed.Add(Trimmed);
		}
	}
	if (Changed.Num() == 0)
	{
		return true;
	}

	TArray<FString> AbsoluteNewer;
	for (const FString& RelativeFile : RelativeFiles)
	{
		FString Normalized = RelativeFile;
		FPaths::NormalizeFilename(Normalized);
		if (!Changed.Contains(Normalized))
		{
			continue;
		}
		FString AbsoluteFile = FPaths::Combine(Root, RelativeFile);
		FPaths::NormalizeFilename(AbsoluteFile);
		AbsoluteNewer.Add(AbsoluteFile);
		InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(TEXT("Check out refused: '%s' has a newer revision on %s. Sync first."), *RelativeFile, *Upstream));
	}
	if (AbsoluteNewer.Num() == 0)
	{
		return true;
	}
	GitSourceControlUtils::CollectNewStates(AbsoluteNewer, OutStates, EFileState::Unset, ETreeState::Unset, ELockState::Unset, ERemoteState::NotAtHead);
	return false;
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	if (!InCommand.bUsingGitLfsLocking)
	{
		InCommand.bCommandSuccessful = false;
		return InCommand.bCommandSuccessful;
	}

	// lock files: execute the LFS command on relative filenames
	const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToGitRoot);

	const TArray<FString>& LockableRelativeFiles = RelativeFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);

	if (LockableRelativeFiles.Num() < 1)
	{
		InCommand.bCommandSuccessful = true;
		return InCommand.bCommandSuccessful;
	}

	if (!EnsureFilesAtRemoteHead(InCommand, States))
	{
		InCommand.bCommandSuccessful = false;
		return InCommand.bCommandSuccessful;
	}

	const bool bSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("lock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), LockableRelativeFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	InCommand.bCommandSuccessful = bSuccess;
	const FString& LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();
	if (bSuccess)
	{
		TArray<FString> AbsoluteFiles;
		for (const auto& RelativeFile : RelativeFiles)
		{
			FString AbsoluteFile = FPaths::Combine(InCommand.PathToGitRoot, RelativeFile);
			FGitLockedFilesCache::AddLockedFile(AbsoluteFile, LockUser);
			FPaths::NormalizeFilename(AbsoluteFile);
			AbsoluteFiles.Add(AbsoluteFile);
		}

		GitSourceControlUtils::CollectNewStates(AbsoluteFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto& State : States)
		{
			State.Value.LockUser = LockUser;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

static FText ParseCommitResults(const TArray<FString>& InResults)
{
	if (InResults.Num() >= 1)
	{
		const FString& FirstLine = InResults[0];
		return FText::Format(LOCTEXT("CommitMessage", "Commited {0}."), FText::FromString(FirstLine));
	}
	return LOCTEXT("CommitMessageUnknown", "Submitted revision.");
}

FName FGitCheckInWorker::GetName() const
{
	return "CheckIn";
}

const FText EmptyCommitMsg;

bool FGitCheckInWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);

	// make a temp file to place our commit message in
	bool bDoCommit = InCommand.Files.Num() > 0;
	const FText& CommitMsg = bDoCommit ? Operation->GetDescription() : EmptyCommitMsg;
	FGitScopedTempFile CommitMsgFile(CommitMsg);
	if (CommitMsgFile.GetFilename().Len() > 0)
	{
		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();

		if (bDoCommit)
		{
			FString ParamCommitMsgFilename = TEXT("--file=\"");
			ParamCommitMsgFilename += FPaths::ConvertRelativePathToFull(CommitMsgFile.GetFilename());
			ParamCommitMsgFilename += TEXT("\"");
			TArray<FString> CommitParameters {ParamCommitMsgFilename};
			const TArray<FString>& FilesToCommit = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

			// If no files were committed, this is false, so we treat it as if we never wanted to commit in the first place.
			bDoCommit = GitSourceControlUtils::RunCommit(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, CommitParameters,
														FilesToCommit, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}

		// If we commit, we can push up the deleted state to gone
		if (bDoCommit)
		{
			// Remove any deleted files from status cache
			TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
			Provider.GetState(InCommand.Files, LocalStates, EStateCacheUsage::Use);
			for (const auto& State : LocalStates)
			{
				if (State->IsDeleted())
				{
					Provider.RemoveFileFromCache(State->GetFilename());
				}
			}
			Operation->SetSuccessMessage(ParseCommitResults(InCommand.ResultInfo.InfoMessages));
			const FString& Message = (InCommand.ResultInfo.InfoMessages.Num() > 0) ? InCommand.ResultInfo.InfoMessages[0] : TEXT("");
			UE_LOG(LogSourceControl, Log, TEXT("commit successful: %s"), *Message);
			GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
		}

		// Collect difference between the remote and what we have on top of remote locally. This is to handle unpushed commits other than the one we just did.
		// Doesn't matter that we're not synced. Because our local branch is always based on the remote.
		TArray<FString> CommittedFiles;
		FString BranchName;
		bool bDiffSuccess;
		if (GitSourceControlUtils::GetRemoteBranchName(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, BranchName))
		{
			TArray<FString> Parameters {"--name-only", FString::Printf(TEXT("%s...HEAD"), *BranchName), "--"};
			bDiffSuccess = GitSourceControlUtils::RunCommand(TEXT("diff"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameters,
															  FGitSourceControlModule::GetEmptyStringArray(), CommittedFiles, InCommand.ResultInfo.ErrorMessages);
		}
		else
		{
			// Get all non-remote commits and list out their files
			TArray<FString> Parameters {"--branches", "--not" "--remotes", "--name-only", "--pretty="};
			bDiffSuccess = GitSourceControlUtils::RunCommand(TEXT("log"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), CommittedFiles, InCommand.ResultInfo.ErrorMessages);
			// Dedup files list between commits
			CommittedFiles = TSet<FString>{CommittedFiles}.Array();
		}

		bool bUnpushedFiles;
		TSet<FString> FilesToCheckIn {InCommand.Files};
		if (bDiffSuccess)
		{
			// Only push if we have a difference (any commits at all, not just the one we just did)
			bUnpushedFiles = CommittedFiles.Num() > 0;
			CommittedFiles = GitSourceControlUtils::AbsoluteFilenames(CommittedFiles, InCommand.PathToRepositoryRoot);
			FilesToCheckIn.Append(CommittedFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable));
		}
		else
		{
			// Be cautious, try pushing anyway
			bUnpushedFiles = true;
		}

		TArray<FString> PulledFiles;

		// If we have unpushed files, push
		if (bUnpushedFiles)
		{
			// TODO: configure remote
			TArray<FString> PushParameters {TEXT("-u"), TEXT("origin"), TEXT("HEAD")};
			InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("push"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot,
																			 PushParameters, FGitSourceControlModule::GetEmptyStringArray(),
																			 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

			if (!InCommand.bCommandSuccessful)
			{
				// if out of date, pull first, then try again
				bool bWasOutOfDate = false;
				for (const auto& PushError : InCommand.ResultInfo.ErrorMessages)
				{
					if ((PushError.Contains(TEXT("[rejected]")) && (PushError.Contains(TEXT("non-fast-forward")) || PushError.Contains(TEXT("fetch first")))) ||
						PushError.Contains(TEXT("cannot lock ref")))
					{
						// Don't do it during iteration, want to append pull results to InCommand.ResultInfo.ErrorMessages
						bWasOutOfDate = true;
						break;
					}
				}
				if (bWasOutOfDate)
				{
					// Get latest
					const bool bFetched = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false,
																			 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
					if (bFetched)
					{
						// Update local with latest
						const bool bPulled = GitSourceControlUtils::PullOrigin(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot,
																			   FGitSourceControlModule::GetEmptyStringArray(), PulledFiles,
																			   InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						if (bPulled)
						{
							InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(
								TEXT("push"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, PushParameters,
								FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						}
					}

					// Our push still wasn't successful
					if (!InCommand.bCommandSuccessful)
					{
						if (!Provider.bPendingRestart)
						{
							// If it fails, just let the user do it
							FText PushFailMessage(LOCTEXT("GitPush_OutOfDate_Msg", "Git Push failed because there are changes you need to pull.\n\n"
																				   "An attempt was made to pull, but failed, because while the Unreal Editor is "
																				   "open, files cannot always be updated.\n\n"
																				   "Please exit the editor, and update the project again."));
							FText PushFailTitle(LOCTEXT("GitPush_OutOfDate_Title", "Git Pull Required"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, PushFailTitle);
#else
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, &PushFailTitle);
#endif
							UE_LOG(LogSourceControl, Log, TEXT("Push failed because we're out of date, prompting user to resolve manually"));
						}
					}
				}
			}
		}
		else
		{
			InCommand.bCommandSuccessful = true;
		}

		// git-lfs: unlock files
		if (InCommand.bUsingGitLfsLocking)
		{
			// If we successfully pushed (or didn't need to push), unlock the files marked for check in
			if (InCommand.bCommandSuccessful)
			{
				// unlock files: execute the LFS command on relative filenames
				// (unlock only locked files, that is, not Added files)
				TArray<FString> LockedFiles;
				GitSourceControlUtils::GetLockedFiles(FilesToCheckIn.Array(), LockedFiles);
				if (LockedFiles.Num() > 0)
				{
					const TArray<FString>& FilesToUnlock = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);

					if (FilesToUnlock.Num() > 0)
					{
						// Not strictly necessary to succeed, so don't update command success
						const bool bUnlockSuccess = GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary,
																						 FGitSourceControlModule::GetEmptyStringArray(), FilesToUnlock,
																						 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
						if (bUnlockSuccess)
						{
							for (const auto& File : LockedFiles)
							{
								FGitLockedFilesCache::RemoveLockedFile(File);
							}
						}
					}
				}
#if 0
				for (const FString& File : FilesToCheckIn.Array())
				{
					FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, true);
				}
#endif
			}
		}

		// Collect all the files we touched through the pull update
		if (bUnpushedFiles && PulledFiles.Num())
		{
			FilesToCheckIn.Append(PulledFiles);
		}
		// Before, we added only lockable files from CommittedFiles. But now, we want to update all files, not just lockables.
		FilesToCheckIn.Append(CommittedFiles);

		// now update the status of our files
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
															   FilesToCheckIn.Array(), InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		return InCommand.bCommandSuccessful;
	}

	InCommand.bCommandSuccessful = false;

	return false;
}

bool FGitCheckInWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitMarkForAddWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitDeleteWorker::GetName() const
{
	return "Delete";
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Deleted, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitDeleteWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}


// Get lists of Missing files (ie "deleted"), Modified files, and "other than Added" Existing files
void GetMissingVsExistingFiles(const TArray<FString>& InFiles, TArray<FString>& OutMissingFiles, TArray<FString>& OutAllExistingFiles, TArray<FString>& OutOtherThanAddedExistingFiles)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const TArray<FString> Files = (InFiles.Num() > 0) ? (InFiles) : (Provider.GetFilesInCache());

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (FPaths::FileExists(State->GetFilename()))
		{
			if (State->IsAdded())
			{
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->IsModified())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->CanRevert()) // for locked but unmodified files
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
		else
		{
			// If already queued for deletion, don't try to delete again
			if (State->IsSourceControlled() && !State->IsDeleted())
			{
				OutMissingFiles.Add(State->GetFilename());
			}
		}
	}
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = true;

	// Filter files by status
	TArray<FString> MissingFiles;
	TArray<FString> AllExistingFiles;
	TArray<FString> OtherThanAddedExistingFiles;
	GetMissingVsExistingFiles(InCommand.Files, MissingFiles, AllExistingFiles, OtherThanAddedExistingFiles);

	const bool bRevertAll = InCommand.Files.Num() < 1;
	if (bRevertAll)
	{
		TArray<FString> Parms;
		Parms.Add(TEXT("--hard"));
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

		Parms.Reset(2);
		Parms.Add(TEXT("-f")); // force
		Parms.Add(TEXT("-d")); // remove directories
		InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("clean"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parms, FGitSourceControlModule::GetEmptyStringArray(), InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else
	{
		if (MissingFiles.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from revision control
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), MissingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (AllExistingFiles.Num() > 0)
		{
			// reset and revert any changes already added to the index
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), AllExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), AllExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
		if (OtherThanAddedExistingFiles.Num() > 0)
		{
			// revert any changes in working copy (this would fails if the asset was in "Added" state, since after "reset" it is now "untracked")
			// may need to try a few times due to file locks from prior operations
			bool CheckoutSuccess = false;
			int32 Attempts = 10;
			while( Attempts-- > 0 )
			{
				CheckoutSuccess = GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), OtherThanAddedExistingFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
				if (CheckoutSuccess)
				{
					break;
				}

				FPlatformProcess::Sleep(0.1f);
			}
			
			InCommand.bCommandSuccessful &= CheckoutSuccess;
		}
	}

	if (InCommand.bUsingGitLfsLocking)
	{
		// unlock files: execute the LFS command on relative filenames
		// (unlock only locked files, that is, not Added files)
		TArray<FString> LockedFiles;
		GitSourceControlUtils::GetLockedFiles(OtherThanAddedExistingFiles, LockedFiles);
		if (LockedFiles.Num() > 0)
		{
			const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, InCommand.PathToGitRoot);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunLFSCommand(TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary, FGitSourceControlModule::GetEmptyStringArray(), RelativeFiles,
																				 InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
			if (InCommand.bCommandSuccessful)
			{
				for (const auto& File : LockedFiles)
				{
					FGitLockedFilesCache::RemoveLockedFile(File);
				}
			}
		}
	}

	// If no files were specified (full revert), refresh all relevant files instead of the specified files (which is an empty list in full revert)
	// This is required so that files that were "Marked for add" have their status updated after a full revert.
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (InCommand.Files.Num() <= 0)
	{
		for (const auto& File : MissingFiles) FilesToUpdate.Add(File);
		for (const auto& File : AllExistingFiles) FilesToUpdate.Add(File);
		for (const auto& File : OtherThanAddedExistingFiles) FilesToUpdate.Add(File);
	}

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, FilesToUpdate, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	return InCommand.bCommandSuccessful;
}

bool FGitRevertWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitSyncWorker::GetName() const
{
	return "Sync";
}

bool FGitSyncWorker::Execute(FGitSourceControlCommand& InCommand)
{
	TArray<FString> Results;
	const bool bFetched = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	if (!bFetched)
	{
		return false;
	}

	InCommand.bCommandSuccessful = GitSourceControlUtils::PullOrigin(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.Files, InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																 InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	return InCommand.bCommandSuccessful;
}

bool FGitSyncWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitFetch::GetName() const
{
	return "Fetch";
}

FText FGitFetch::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Fetching from remote origin...");
}

FName FGitFetchWorker::GetName() const
{
	return "Fetch";
}

bool FGitFetchWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = GitSourceControlUtils::FetchRemote(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																	  InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		return false;
	}

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FGitFetch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FGitFetch>(InCommand.Operation);

	if (Operation->bUpdateStatus)
	{
		// Now update the status of all our files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();

		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking,
																			  ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitFetchWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	if(InCommand.Files.Num() > 0)
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
			if (Operation->ShouldUpdateHistory())
			{
				for (const auto& State : UpdatedStates)
				{
					const FString& File = State.Key;
					TGitSourceControlHistory History;

					if (State.Value.IsConflicted())
					{
						// In case of a merge conflict, we first need to get the tip of the "remote branch" (MERGE_HEAD)
						GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, true,
															 InCommand.ResultInfo.ErrorMessages, History);
					}
					// Get the history of the file in the current branch
					InCommand.bCommandSuccessful &= GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, false,
																						 InCommand.ResultInfo.ErrorMessages, History);
					Histories.Add(*File, History);
				}
			}
		}
	}
	else
	{
		// no path provided: only update the status of assets in Content/ directory and also Config files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();
		
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, ProjectDirs, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = Now;
		bUpdated = true;
	}

	return bUpdated;
}

FName FGitCopyWorker::GetName() const
{
	return "Copy";
}

bool FGitCopyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	// Copy or Move operation on a single file : Git does not need an explicit copy nor move,
	// but after a Move the Editor create a redirector file with the old asset name that points to the new asset.
	// The redirector needs to be committed with the new asset to perform a real rename.
	// => the following is to "MarkForAdd" the redirector, but it still need to be committed by selecting the whole directory and "check-in"
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Added, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCopyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitResolveWorker::GetName() const
{
	return "Resolve";
}

bool FGitResolveWorker::Execute( class FGitSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	// mark the conflicting files as resolved:
	TArray<FString> Results;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, Results, InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitResolveWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

#if ENGINE_MAJOR_VERSION == 5
FName FGitMoveToChangelistWorker::GetName() const
{
	return "MoveToChangelist";
}

bool FGitMoveToChangelistWorker::UpdateStates() const
{
	return true;
}

bool FGitMoveToChangelistWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	FGitSourceControlChangelist DestChangelist = InCommand.Changelist;
	bool bResult = false;
	if(DestChangelist.GetName().Equals(TEXT("Staged")))
	{
		bResult = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	else if(DestChangelist.GetName().Equals(TEXT("Working")))
	{
		TArray<FString> Parameter;
		Parameter.Add(TEXT("--staged"));
		bResult = GitSourceControlUtils::RunCommand(TEXT("restore"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, Parameter, InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
	}
	
	if (bResult)
	{
		TMap<FString, FGitSourceControlState> DummyStates;
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ResultInfo.InfoMessages, DummyStates);
	}
	return bResult;
}

FName FGitUpdateStagingWorker::GetName() const
{
	return "UpdateChangelistsStatus";
}

bool FGitUpdateStagingWorker::Execute(FGitSourceControlCommand& InCommand)
{
	return GitSourceControlUtils::UpdateChangelistStateByCommand();
}

bool FGitUpdateStagingWorker::UpdateStates() const
{
	return true;
}
#endif

#undef LOCTEXT_NAMESPACE
