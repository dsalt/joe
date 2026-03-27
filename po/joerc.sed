#! /bin/sed -r
# Script for mangling joerc for parsing by xgettext

# Ignore the start marker for the root menu
/^menu,"root",rtn/ b;

# Menu definition: mark up the menu title
#/^:defmenu / {
#	s/([^"]*)"(.*)".*$/\1_("\2")/
#	b
#}

# Mark up the menu item text and the text of any 'msg' commands
/^(menu|mode)/ {
	s/^(.*\t)(.*)$/\1_("\2")/;
	/msg,/ s/msg,("[^"]+")/msg,_(\1)/;
	b;
};

# Mark up help text blocks
/^\{[[:upper:]][[:alnum:] ]*$/ {
	/^\{CharTable/ b ignore; # not processing this one
	n;
	:loop
	N;
	/\n}/! b loop;
	s/[\\"]/\\&/g;
	s/^/_("/;
	s/\n/\\n"\n"/g;
	s/"\n"}$/")\n}/;
	b;
};
:ignore

# Stop xgettext warnings about unterminated constants
s/['"]/’/g
# Ensure that there are no other string markers
s/_\(/.(/g
