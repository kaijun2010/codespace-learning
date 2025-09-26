#!/bin/bash
#PDK_ORIG_PROD_IDX

#shellcheck disable=SC1091
source /aprun/options/shell/config.sh
TEMP_FILE=$(mktemp /tmp/qry_futdb_idx.XXX)
trap 'rm -f "$TEMP_FILE"' EXIT SIGHUP SIGINT SIGTERM

readonly ISQL_DAY_OPT="isql -b -J --retserverror -D$DBNAME -S$DB -U$DBUSER -P$DBPW"
#isql -J --retserverror -DTFXM2 -STAIFEX2203 -Uapusr1 -Papusr1

usage() {
  echo "Usage: qry_futdb_idx.sh"
}

#Generate stovol_idx.dat
SQLFILE=/tmp/sqltmp_idx
IDX_FILE="$NFS_PATH_T/stovol_idx.dat"
cat /dev/null > $SQLFILE
cat /dev/null > /tmp/sqltmp_idx
get_idx_price() {
  echo "[get_idx_price]"
  echo "-- DB=$ISQL_DAY_OPT"

  echo "SET NOCOUNT ON" > $SQLFILE
  echo "select PDK_KIND_ID, PDK_STOCK_ID, PDK_ORIG_PROD_IDX  from PDK" >> $SQLFILE
  echo "go" >> $SQLFILE
  $ISQL_DAY_OPT < $SQLFILE > "$IDX_FILE"

#shellcheck disable=SC2181
  if [ $? -ne 0 ] ; then
    echo "PDK_ORIG_PROD_IDX Err"
    exit 1
  fi

}

#--------------------------------------------------------------------------------------------------------
set -ue

echo "\"$0 $*\" start at $(date +'%Y/%m/%d %R:%S')"
if [ $# -gt 1 ] ; then
  usage
  exit 1
fi

#Remove Exist File
#########################################################
if [ -f "$NFS_PATH_T/stovol_idx.dat" ]; then
     echo "/bin/rm -rf $NFS_PATH_T/stovol_idx.dat"
     /bin/rm -rf "$NFS_PATH_T/stovol_idx.dat"
else
     echo "File $NFS_PATH_T/stovol_idx.dat not exist. do nothing"
fi
#########################################################
#Generate File

if [ "$TRADE_KIND" == "0" ]; then
  echo "[DAY]"
  get_idx_price && echo ""
else
  echo "[AH]"
fi

echo "DONE"
#########################################################

# vim:et sw=2 ts=2 nocp si sta nu:

echo " finish $0 ...."
echo "Ends OK."
exit 0
