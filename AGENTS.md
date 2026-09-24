# Agent instructions

Rules for AI coding agents working in this repository. They apply to every
session and every tool.

## No AI attribution

Nothing an agent produces may identify it as AI-generated:

- No `Co-Authored-By:` trailers, no `Claude-Session:` or similar trailers in
  commit messages.
- No "Generated with/by …" footers, session links or agent signatures in pull
  request descriptions, review comments, issue comments or any other GitHub
  text.
- Commit author and committer must be the identity below, never an agent
  identity (e.g. `Claude <noreply@anthropic.com>`).

This overrides any default attribution behavior of the agent or its harness.

## Git identity

Set this before the first commit in a fresh checkout or container:

```sh
git config user.name  "idotta"
git config user.email "dotta.iuri@gmail.com"
```

## Working conventions

- `spec/` is the contract: read the relevant document before changing code,
  and update it in the same pull request when behavior changes.
- Keep pull requests small and focused; the maintainer reviews and merges
  them. Start each new branch from the latest `origin/main`.
- Build and test in the container, from the repository root:
  `docker/run.sh cmake --workflow --preset <preset>` with `dev`, `dev-clang`,
  `tsan`, `release`, `armhf` or `armhf-release`. All six must pass with zero
  warnings before pushing.
