# See the file LICENSE for redistribution information.
#
# Copyright (c) 1996, 1997, 1998, 1999
#	Sleepycat Software.  All rights reserved.
#
#	@(#)test015.tcl	11.13 (Sleepycat) 10/26/99
#
# DB Test 15 {access method}
# Partial put test when item does not exist.
proc test015 { method {nentries 7500} { start 0 } args } {
	global fixed_len

	set low_range 50
	set mid_range 100
	set high_range 1000

	if { [is_fixed_length $method] } {
		set low_range [expr $fixed_len/2 - 2]
		set mid_range [expr $fixed_len/2]
		set high_range $fixed_len
	}

	set t_table {
		{ 1 { 1 1 1 } }
		{ 2 { 1 1 5 } }
		{ 3 { 1 1 $low_range } }
		{ 4 { 1 $mid_range 1 } }
		{ 5 { $mid_range $high_range 5 } }
		{ 6 { 1 $mid_range $low_range } }
	}

	puts "Test015: \
	    $method ($args) $nentries equal key/data pairs, partial put test"
	test015_init
	if { $start == 0 } {
		set start { 1 2 3 4 5 6 }
	}
	foreach entry $t_table {
		set this [lindex $entry 0]
		if { [lsearch $start $this] == -1 } {
			continue
		}
		puts -nonewline "$this: "
		eval [concat test015_body $method [lindex $entry 1] \
		    $nentries $args]
	}
}

proc test015_init { } {
	global rand_init

	berkdb srand $rand_init
}

proc test015_body { method off_low off_hi rcount {nentries 10000} args } {
	global dvals
	global fixed_len
	source ./include.tcl

	set args [convert_args $method $args]
	set omethod [convert_method $method]

	set checkfunc test015.check

	if { [is_fixed_length $method] && \
		[string compare $omethod "-recno"] == 0} {
		# is fixed recno method
		set checkfunc test015.check
	}

	puts "Put $rcount strings random offsets between $off_low and $off_hi"

	# Create the database and open the dictionary
	set testfile $testdir/test015.db
	set t1 $testdir/t1
	set t2 $testdir/t2
	set t3 $testdir/t3
	cleanup $testdir

	set db [eval {berkdb \
	    open -create -truncate -mode 0644} $args {$omethod $testfile}]
	error_check_good dbopen [is_valid_db $db] TRUE

	set pflags ""
	set gflags ""
	set txn ""
	set count 0

	puts "\tTest015.a: put/get loop"

	# Here is the loop where we put and get each key/data pair
	# Each put is a partial put of a record that does not exist.
	set did [open $dict]
	while { [gets $did str] != -1 && $count < $nentries } {
		if { [is_record_based $method] == 1 } {
			if { [string length $str] > $fixed_len } {
				continue
			}
			set key [expr $count + 1]
		} else {
			set key $str
		}

		if { 0 } {
			set data [replicate $str $rcount]
			set off [ berkdb random_int $off_low $off_hi ]
			set offn [expr $off + 1]
			if { [is_fixed_length $method] && \
			    [expr [string length $data] + $off] >= $fixed_len} {
			    set data [string range $data 0 [expr $fixed_len-$offn]]
			}
			set dvals($key) [partial_shift $data $off right]
		} else {
			set data [chop_data $method [replicate $str $rcount]]

			# This is a hack.  In DB we will store the records with
			# some padding, but these will get lost if we just return
			# them in TCL.  As a result, we're going to have to hack
			# get to check for 0 padding and return a list consisting
			# of the number of 0's and the actual data.
			set off [ berkdb random_int $off_low $off_hi ]

			# There is no string concatenation function in Tcl
			# (although there is one in TclX), so we have to resort
			# to this hack. Ugh.
			set slen [string length $data]
			if {[is_fixed_length $method] && \
			    $slen > $fixed_len - $off} {
				set $slen [expr $fixed_len - $off]
			}
			set a "a"
			set dvals($key) [pad_data \
			    $method [eval "binary format x$off$a$slen" {$data}]]
		}
		if {[is_fixed_length $method] && \
		    [string length $data] > ($fixed_len - $off)} {
		    set slen [expr $fixed_len - $off]
		    set data [eval "binary format a$slen" {$data}]
		}
		set ret [eval {$db put} \
		    {-partial [list $off [string length $data]] $key $data}]
		error_check_good put $ret 0

		incr count
	}
	close $did

	# Now make sure that everything looks OK
	puts "\tTest015.b: check entire file contents"
	dump_file $db $txn $t1 $checkfunc
	error_check_good db_close [$db close] 0

	# Now compare the keys to see if they match the dictionary (or ints)
	if { [is_record_based $method] == 1 } {
		set oid [open $t2 w]
		for {set i 1} {$i <= $nentries} {set i [incr i]} {
			puts $oid $i
		}
		close $oid
		exec $SORT $t2 > $t3
		exec $MV $t3 $t2
		exec $SORT $t1 > $t3
	} else {
		set q q
		exec $SED $nentries$q $dict > $t3
		exec $SORT $t3 > $t2
		exec $SORT $t1 > $t3
	}

	error_check_good Test015:diff($t3,$t2) \
	    [catch { exec $CMP $t3 $t2 } res] 0

	puts "\tTest015.c: close, open, and dump file"
	# Now, reopen the file and run the last test again.
	open_and_dump_file $testfile NULL $txn $t1 \
	    $checkfunc dump_file_direction "-first" "-next"

	if { [string compare $omethod "-recno"] != 0 } {
		exec $SORT $t1 > $t3
	}

	error_check_good Test015:diff($t3,$t2) \
	    [catch { exec $CMP $t3 $t2 } res] 0

	# Now, reopen the file and run the last test again in the
	# reverse direction.
	puts "\tTest015.d: close, open, and dump file in reverse direction"
	open_and_dump_file $testfile NULL $txn $t1 \
	    $checkfunc dump_file_direction "-last" "-prev"

	if { [string compare $omethod "-recno"] != 0 } {
		exec $SORT $t1 > $t3
	}

	error_check_good Test015:diff($t3,$t2) \
	    [catch { exec $CMP $t3 $t2 } res] 0

	unset dvals
}

# Check function for test015; keys and data are identical
proc test015.check { key data } {
	global dvals

	error_check_good key"$key"_exists [info exists dvals($key)] 1
	binary scan $data "c[string length $data]" a
	binary scan $dvals($key) "c[string length $dvals($key)]" b
	error_check_good "mismatch on padding for key $key" $a $b
}

proc test015.fixed.check { key data } {
	global dvals
	global fixed_len

	error_check_good key"$key"_exists [info exists dvals($key)] 1
	if { [string length $data] > $fixed_len } {
		error_check_bad \
		    "data length:[string length $data] \
		    for fixed:$fixed_len" 1 1
	}
	puts "$data : $dvals($key)"
	error_check_good compare_data($data,$dvals($key) \
	    $dvals($key) $data
}
