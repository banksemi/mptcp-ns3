#! /bin/bash

git stash
git pull
# git stash pop # git stash apply; git stash drop;

# stash 리스트 출력
git stash list

echo "If you restore your works, Please enter 'git stash pop'"

./_update.sh
