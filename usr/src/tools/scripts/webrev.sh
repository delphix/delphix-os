#!/usr/bin/ksh93 -p
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
#
# Display remote target with prefix and trailing slash.
#
function print_upload_header
{
	typeset -r prefix=$1
	typeset display_target

	if [[ -z $tflag ]]; then
		display_target=${prefix}${remote_target}
	else
		display_target=${remote_target}
	fi

	if [[ ${display_target} != */ ]]; then
		display_target=${display_target}/
	fi

	print "      Upload to: ${display_target}\n" \
	    "     Uploading: \c"
}

#
#
	if (( $# != 2 )); then
		print "\nERROR: rsync_upload: wrong usage ($#)"
		exit 1
	typeset -r dst=$1
	integer -r print_err_msg=$2
	print_upload_header ${rsync_prefix}
	print "rsync ... \c"
	typeset -r err_msg=$( $MKTEMP /tmp/rsync_err.XXX )
	if [[ -z $err_msg ]]; then
		print "\nERROR: rsync_upload: cannot create temporary file"
		return 1
	fi
	#
	# The source directory must end with a slash in order to copy just
	# directory contents, not the whole directory.
	#
	typeset src_dir=$WDIR
	if [[ ${src_dir} != */ ]]; then
		src_dir=${src_dir}/
	fi
	$RSYNC -r -q ${src_dir} $dst 2>$err_msg
		if (( ${print_err_msg} > 0 )); then
			print "Failed.\nERROR: rsync failed"
			print "src dir: '${src_dir}'\ndst dir: '$dst'"
			print "error messages:"
			$SED 's/^/> /' $err_msg
			rm -f $err_msg
		fi
	rm -f $err_msg
#
# Create directories on remote host using SFTP. Return 0 on success,
# 1 on failure.
#
function remote_mkdirs
{
	typeset -r dir_spec=$1

	#
	# If the supplied path is absolute we assume all directories are
	# created, otherwise try to create all directories in the path
	# except the last one which will be created by scp.
	#
	if [[ "${dir_spec}" == */* && "${dir_spec}" != /* ]]; then
		print "mkdirs \c"
		#
		# Remove the last directory from directory specification.
		#
		typeset -r dirs_mk=${dir_spec%/*}
		typeset -r batch_file_mkdir=$( $MKTEMP /tmp/webrev_mkdir.XXX )
		if [[ -z $batch_file_mkdir ]]; then
			print "\nERROR: remote_mkdirs:" \
			    "cannot create temporary file for batch file"
			return 1
		fi
                OLDIFS=$IFS
                IFS=/
		typeset dir
                for dir in ${dirs_mk}; do
			#
			# Use the '-' prefix to ignore mkdir errors in order
			# to avoid an error in case the directory already
			# exists. We check the directory with chdir to be sure
			# there is one.
			#
                        print -- "-mkdir ${dir}" >> ${batch_file_mkdir}
                        print "chdir ${dir}" >> ${batch_file_mkdir}
                done
                IFS=$OLDIFS
		typeset -r sftp_err_msg=$( $MKTEMP /tmp/webrev_scp_err.XXX )
		if [[ -z ${sftp_err_msg} ]]; then
			print "\nERROR: remote_mkdirs:" \
			    "cannot create temporary file for error messages"
			return 1
		fi
		$SFTP -b ${batch_file_mkdir} ${host_spec} 2>${sftp_err_msg} 1>&2
		if (( $? != 0 )); then
			print "\nERROR: failed to create remote directories"
			print "error messages:"
			$SED 's/^/> /' ${sftp_err_msg}
			rm -f ${sftp_err_msg} ${batch_file_mkdir}
			return 1
		fi
		rm -f ${sftp_err_msg} ${batch_file_mkdir}
	fi

	return 0
}

#
#
		print "\nERROR: ssh_upload: wrong number of arguments"
		exit 1
	#
	# Display the upload information before calling delete_webrev
	# because it will also print its progress.
	#
	print_upload_header ${ssh_prefix}

	#
	# If the deletion was explicitly requested there is no need
	# to perform it again.
	#
		#
		# We do not care about return value because this might be
		# the first time this directory is uploaded.
		#
	#
	# Create remote directories. Any error reporting will be done
	# in remote_mkdirs function.
	#
	remote_mkdirs ${dir_spec}
	if (( $? != 0 )); then
		return 1
	print "upload ... \c"
	typeset -r scp_err_msg=$( $MKTEMP /tmp/scp_err.XXX )
	if [[ -z ${scp_err_msg} ]]; then
		print "\nERROR: ssh_upload:" \
		    "cannot create temporary file for error messages"
		return 1
	fi
		$WDIR $dst 2>${scp_err_msg}
		print "Failed.\nERROR: scp failed"
		print "src dir: '$WDIR'\ndst dir: '$dst'"
		print "error messages:"
		$SED 's/^/> /' ${scp_err_msg}
		rm -f ${scp_err_msg}
	rm -f ${scp_err_msg}
# on failure. If first argument is 1 then perform the check of sftp return
# value otherwise ignore it. If second argument is present it means this run
# only performs deletion.
	if (( $# < 1 )); then
		print "delete_webrev: wrong number of arguments"
		exit 1
	integer -r check=$1
	integer delete_only=0
	if (( $# == 2 )); then
		delete_only=1
	fi

	#
	#
	#
	#
	#
	#
	if (( ${delete_only} > 0 )); then
		print "       Removing: \c"
	else
		print "rmdir \c"
	fi
		print "\nERROR: empty directory for removal"
	#
	#
		print "\nERROR: delete_webrev: cannot create temporary file"
	#
	#
	typeset -r sftp_err_msg=$( $MKTEMP /tmp/webrev_scp_err.XXX )
	if [[ -z ${sftp_err_msg} ]]; then
		print "\nERROR: delete_webrev:" \
		    "cannot create temporary file for error messages"
		return 1
	fi
	$SFTP -b $batch_file_rm $host_spec 2>${sftp_err_msg} 1>&2
		print "Failed.\nERROR: failed to remove remote directories"
		print "error messages:"
		$SED 's/^/> /' ${sftp_err_msg}
		rm -f ${sftp_err_msg}
	rm -f ${sftp_err_msg}
	if (( ${delete_only} > 0 )); then
		print "Done."
	fi
	integer ret
		print "\nERROR: webrev directory '$WDIR' does not exist"
	#
	#
			print "\nERROR: directory '$WDIR' contains" \
			    "\"closed\" directory"

	#
	# We have the URI for remote destination now so let's start the upload.
	#
			rsync_upload ${remote_target##$rsync_prefix} 1
			ret=$?
			return $ret
			ret=$?
			return $ret
		#
		# Try rsync first and fallback to SSH in case it fails.
		#
		rsync_upload ${remote_target} 0
		ret=$?
		if (( $ret != 0 )); then
			print "Failed. (falling back to SSH)"
			ret=$?
		return $ret
# input_cmd | url_encode | output_cmd
# URL-encode (percent-encode) reserved characters as defined in RFC 3986.
# Reserved characters are: :/?#[]@!$&'()*+,;=
#
# While not a reserved character itself, percent '%' is reserved by definition
# so encode it first to avoid recursive transformation, and skip '/' which is
# a path delimiter.
#
function url_encode
	$SED -e "s|%|%25|g" -e "s|:|%3A|g" -e "s|\&|%26|g" \
	    -e "s|?|%3F|g" -e "s|#|%23|g" -e "s|\[|%5B|g" \
	    -e "s|*|%2A|g" -e "s|@|%40|g" -e "s|\!|%21|g" \
	    -e "s|=|%3D|g" -e "s|;|%3B|g" -e "s|\]|%5D|g" \
	    -e "s|(|%28|g" -e "s|)|%29|g" -e "s|\'|%27|g" \
	    -e "s|+|%2B|g" -e "s|\,|%2C|g" -e "s|\\\$|%24|g"
# input_cmd | html_quote | output_cmd
# or
# html_quote filename | output_cmd
# Make a piece of source code safe for display in an HTML <pre> block.
html_quote()
	$SED -e "s/&/\&amp;/g" -e "s/</\&lt;/g" -e "s/>/\&gt;/g" "$@" | expand
# input_cmd | its2url | output_cmd
# Scan for information tracking system references and insert <a> links to the
# relevant databases.
its2url()
	$SED -f ${its_sed_script}
	    <script type="text/javascript" src="${RTOP}ancnav.js"></script>
	  <frame src="${RTOP}ancnav.html" scrolling="no" marginwidth="0"
        typeset cur="${1##$2?(/)}"

        #
        # If the first path was specified absolutely, and it does
        # not start with the second path, it's an error.
        #
        if [[ "$cur" = "/${1#/}" ]]; then
                # Should never happen.
                print -u2 "\nWARNING: relative_dir: \"$1\" not relative "
                print -u2 "to \"$2\".  Check input paths.  Framed webrev "
                print -u2 "will not be relocatable!"
                print $2
                return
        fi

	#
	# This is kind of ugly.  The sed script will do the following:
	#
	# 1. Strip off a leading "." or "./": this is important to get
	#    the correct arcnav links for files in $WDIR.
	# 2. Strip off a trailing "/": this is not strictly necessary, 
	#    but is kind of nice, since it doesn't end up in "//" at
	#    the end of a relative path.
	# 3. Replace all remaining sequences of non-"/" with "..": the
	#    assumption here is that each dirname represents another
	#    level of relative separation.
	# 4. Append a trailing "/" only for non-empty paths: this way
	#    the caller doesn't need to duplicate this logic, and does
	#    not end up using $RTOP/file for files in $WDIR.
	#
	print $cur | $SED -e '{
		s:^\./*::
		s:/$::
		s:[^/][^/]*:..:g
		s:^\(..*\)$:\1/:
	}'
		    html_quote | its2url | $AWK "$nawkprg"
	print -- "$comm" | html_quote | its2url
	eval `$SED -e "s/#.*$//" $FLIST | $GREP = `
	file=`echo $file | $SED 's#/#\\\/#g'`
	old_mode=`$SED -n -e "/^\\(...\\) . ${file}$/s//\\1/p" \
	-C <filename>: Use <filename> for the information tracking configuration.
	-I <filename>: Use <filename> for the information tracking registry.
[[ -z $SED ]] && SED=`look_for_prog sed`
# prefixes for upload targets
typeset -r rsync_prefix="rsync://"
typeset -r ssh_prefix="ssh://"
Cflag=
Iflag=
while getopts "C:tDi:I:lnNo:Op::Uw" opt
	C)	Cflag=1
		ITSCONF=$OPTARG;;

	I)	Iflag=1
		ITSREG=$OPTARG;;

#
# If the command line options indicate no webrev generation, either
# explicitly (-n) or implicitly (-D but not -U), then there's a whole
# ton of logic we can skip.
#
# Instead of increasing indentation, we intentionally leave this loop
# body open here, and exit via break from multiple points within.
# Search for DO_EVERYTHING below to find the break points and closure.
#
for do_everything in 1; do

# DO_EVERYTHING: break point
if [[ -n $nflag || ( -z $Uflag && -n $Dflag ) ]]; then
	break
fi

		eval `$SED -e "s/#.*$//" $wxfile | $GREP HG_PARENT=`
		    eval `$SED -e "s/#.*$//" | $GREP HG_PARENT=`
# DO_EVERYTHING: break point
if [[ -n $Nflag ]]; then
	break
fi

typeset -A itsinfo
typeset -r its_sed_script=/tmp/$$.its_sed
valid_prefixes=
if [[ -z $nflag ]]; then
	DEFREGFILE="$(dirname $(whence $0))/../etc/its.reg"
	if [[ -n $Iflag ]]; then
		REGFILE=$ITSREG
	elif [[ -r $HOME/.its.reg ]]; then
		REGFILE=$HOME/.its.reg
	else
		REGFILE=$DEFREGFILE
	fi
	if [[ ! -r $REGFILE ]]; then
		print "ERROR: Unable to read database registry file $REGFILE"
		exit 1
	elif [[ $REGFILE != $DEFREGFILE ]]; then
		print "   its.reg from: $REGFILE"
	fi

	$SED -e '/^#/d' -e '/^[ 	]*$/d' $REGFILE | while read LINE; do
		
		name=${LINE%%=*}
		value="${LINE#*=}"

		if [[ $name == PREFIX ]]; then
			p=${value}
			valid_prefixes="${p} ${valid_prefixes}"
		else
			itsinfo["${p}_${name}"]="${value}"
		fi
	done


	DEFCONFFILE="$(dirname $(whence $0))/../etc/its.conf"
	CONFFILES=$DEFCONFFILE
	if [[ -r $HOME/.its.conf ]]; then
		CONFFILES="${CONFFILES} $HOME/.its.conf"
	fi
	if [[ -n $Cflag ]]; then
		CONFFILES="${CONFFILES} ${ITSCONF}"
	fi
	its_domain=
	its_priority=
	for cf in ${CONFFILES}; do
		if [[ ! -r $cf ]]; then
			print "ERROR: Unable to read database configuration file $cf"
			exit 1
		elif [[ $cf != $DEFCONFFILE ]]; then
			print "       its.conf: reading $cf"
		fi
		$SED -e '/^#/d' -e '/^[ 	]*$/d' $cf | while read LINE; do
		    eval "${LINE}"
		done
	done

	#
	# If an information tracking system is explicitly identified by prefix,
	# we want to disregard the specified priorities and resolve it accordingly.
	#
	# To that end, we'll build a sed script to do each valid prefix in turn.
	#
	for p in ${valid_prefixes}; do
		#
		# When an informational URL was provided, translate it to a
		# hyperlink.  When omitted, simply use the prefix text.
		#
		if [[ -z ${itsinfo["${p}_INFO"]} ]]; then
			itsinfo["${p}_INFO"]=${p}
		else
			itsinfo["${p}_INFO"]="<a href=\\\"${itsinfo["${p}_INFO"]}\\\">${p}</a>"
		fi

		#
		# Assume that, for this invocation of webrev, all references
		# to this information tracking system should resolve through
		# the same URL.
		#
		# If the caller specified -O, then always use EXTERNAL_URL.
		#
		# Otherwise, look in the list of domains for a matching
		# INTERNAL_URL.
		#
		[[ -z $Oflag ]] && for d in ${its_domain}; do
			if [[ -n ${itsinfo["${p}_INTERNAL_URL_${d}"]} ]]; then
				itsinfo["${p}_URL"]="${itsinfo[${p}_INTERNAL_URL_${d}]}"
				break
			fi
		done
		if [[ -z ${itsinfo["${p}_URL"]} ]]; then
			itsinfo["${p}_URL"]="${itsinfo[${p}_EXTERNAL_URL]}"
		fi

		#
		# Turn the destination URL into a hyperlink
		#
		itsinfo["${p}_URL"]="<a href=\\\"${itsinfo[${p}_URL]}\\\">&</a>"

		print "/^${p}[ 	]/ {
				s;${itsinfo[${p}_REGEX]};${itsinfo[${p}_URL]};g
				s;^${p};${itsinfo[${p}_INFO]};
			}" >> ${its_sed_script}
	done

	#
	# The previous loop took care of explicit specification.  Now use
	# the configured priorities to attempt implicit translations.
	#
	for p in ${its_priority}; do
		print "/^${itsinfo[${p}_REGEX]}[ 	]/ {
				s;${itsinfo[${p}_REGEX]};${itsinfo[${p}_URL]};g
			}" >> ${its_sed_script}
	done
fi

#
# Search for DO_EVERYTHING above for matching "for" statement
# and explanation of this terminator.
#
done

	#
	#
		#
		# Check upload target prefix first.
		#
		if [[ "${remote_target}" != ${rsync_prefix}* &&
		    "${remote_target}" != ${ssh_prefix}* ]]; then
			print "ERROR: invalid prefix of upload URI" \
			    "($remote_target)"
			exit 1
		fi
		#
		#
		typeset target_no_prefix=${remote_target##*://}
		if [[ ${target_no_prefix} == *:* ]]; then
				remote_target=${remote_target}${WNAME}
			fi
		else
			if [[ ${target_no_prefix} == */* ]]; then
				print "ERROR: badly formed upload URI" \
					"($remote_target)"
				exit 1
				remote_target=${remote_target}:${WNAME}

	#
	# Strip trailing slash. Each upload method will deal with directory
	# specification separately.
	#
	remote_target=${remote_target%/}
#
#
	delete_webrev 1 1
#
#
		delete_webrev 1 1
			| $SED -e 's/\([0-9a-f]\{12\}\).*/\1/'`
$SED -e "s/#.*$//" -e "/=/d" -e "/^[   ]*$/d" $FLIST > /tmp/$$.flist.clean
	$SED '
	    $SED -n -f $SEDFILE > $HG_PARENT_MANIFEST
	cleanse_rmfile="$SED 's/^\(@@ [0-9+,-]*\) [0-9+,-]* @@$/\1 +0,0 @@/'"
	cleanse_newfile="$SED 's/^@@ [0-9+,-]* \([0-9+,-]* @@\)$/@@ -0,0 \1/'"
	wpatch_url="$(print $WNAME.patch | url_encode)"
	print "<a href=\"$wpatch_url\">$WNAME.patch</a></td></tr>"
	wpdf_url="$(print $WNAME.pdf | url_encode)"
	print "<a href=\"$wpdf_url\">$WNAME.pdf</a></td></tr>"
		cdiff_url="$(print $P.cdiff.html | url_encode)"
		udiff_url="$(print $P.udiff.html | url_encode)"
		print "<a href=\"$cdiff_url\">Cdiffs</a>"
		print "<a href=\"$udiff_url\">Udiffs</a>"
			wdiff_url="$(print $P.wdiff.html | url_encode)"
			print "<a href=\"$wdiff_url\">Wdiffs</a>"
		sdiff_url="$(print $P.sdiff.html | url_encode)"
		print "<a href=\"$sdiff_url\">Sdiffs</a>"
		frames_url="$(print $P.frames.html | url_encode)"
		print "<a href=\"$frames_url\">Frames</a>"
		oldfile_url="$(print $P-.html | url_encode)"
		print "<a href=\"$oldfile_url\">Old</a>"
		newfile_url="$(print $P.html | url_encode)"
		print "<a href=\"$newfile_url\">New</a>"
		patch_url="$(print $P.patch | url_encode)"
		print "<a href=\"$patch_url\">Patch</a>"
		rawfiles_url="$(print raw_files/new/$P | url_encode)"
		print "<a href=\"$rawfiles_url\">Raw</a>"
#
#
	delete_webrev 1 1