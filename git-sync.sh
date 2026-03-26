#!/bin/bash
# save this as git-sync.sh and run with: ./git-sync.sh "Your commit message"

# Exit if no commit message provided
if [ -z "$1" ]; then
  echo "Usage: ./git-sync.sh \"commit message\""
  exit 1
fi

# Show current status
git status

# Stage all changes
git add .

# Commit with the message you pass in
git commit -m "$1"

# Pull remote changes first (rebase to keep history clean)
git pull origin main --rebase

# Push to GitHub
git push -u origin main
