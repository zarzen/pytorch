from unittest import TestCase, mock, main
from test_trymerge import mocked_gh_graphql
from trymerge import GitHubPR
from gitutils import get_git_remote_name, get_git_repo_dir, GitRepo
from typing import Any
from tryrebase import rebase_onto


class TestRebase(TestCase):

    @mock.patch('trymerge.gh_graphql', side_effect=mocked_gh_graphql)
    @mock.patch('gitutils.GitRepo._run_git')
    @mock.patch('tryrebase.gh_post_comment')
    def test_rebase(self, mocked_post_comment: Any, mocked_run_git: Any, mocked_gql: Any) -> None:
        "Tests rebase successfully"
        pr = GitHubPR("pytorch", "pytorch", 31093)
        repo = GitRepo(get_git_repo_dir(), get_git_remote_name())
        rebase_onto(pr, repo)
        calls = [mock.call('fetch', 'origin', 'pull/31093/head:pull/31093/head'),
                 mock.call('rebase', 'master', 'pull/31093/head'),
                 mock.call('push', '-f', 'https://github.com/mingxiaoh/pytorch.git', 'pull/31093/head:master')]
        mocked_run_git.assert_has_calls(calls)
        self.assertTrue("Successfully rebased `master` onto `master`" in mocked_post_comment.call_args[0][3])

    @mock.patch('trymerge.gh_graphql', side_effect=mocked_gh_graphql)
    @mock.patch('gitutils.GitRepo._run_git', return_value="Everything up-to-date")
    @mock.patch('tryrebase.gh_post_comment')
    def test_no_need_to_rebase(self, mocked_post_comment: Any, mocked_run_git: Any, mocked_gql: Any) -> None:
        "Tests branch already up to date"
        pr = GitHubPR("pytorch", "pytorch", 31093)
        repo = GitRepo(get_git_repo_dir(), get_git_remote_name())
        rebase_onto(pr, repo)
        calls = [mock.call('fetch', 'origin', 'pull/31093/head:pull/31093/head'),
                 mock.call('rebase', 'master', 'pull/31093/head'),
                 mock.call('push', '-f', 'https://github.com/mingxiaoh/pytorch.git', 'pull/31093/head:master')]
        mocked_run_git.assert_has_calls(calls)
        self.assertTrue(
            "Tried to rebase and push PR #31093, but it was already up to date" in mocked_post_comment.call_args[0][3])


if __name__ == "__main__":
    main()
