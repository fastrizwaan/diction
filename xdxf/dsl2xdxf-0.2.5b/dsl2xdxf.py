#!/usr/bin/env python
"""*************************************************************************
 *   Copyright (C) 2005 by Alexander Goryachev                             *
 *   thorn_st@users.sourceforge.net                                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
  ***************************************************************************"""

import sys, re, string, getopt, os, site, codecs, tempfile, shutil
from locale import getdefaultlocale
from stat import *

Version = '0.2.5b'

ProgramName = os.path.basename(sys.argv[0])

FromLanguage = ToLanguage = Name = AbbrevFile = ''
NL = '' #new line simbol
TB = '' #tab simbol

Encodings = { #encodings supported by Lingvo v.8 (apart from unicode)
    'latin':'CP1252',
    'cyrillic':'CP1251', #default if not unicode, and there's no #SOURCE_CODE_PAGE tag
    'easterneuropean':'CP1250'}

LongLanguages = { #languages supported by Lingvo v.8
    'afrikaans':'Afrikaans',
    'basque':'Basque',
    'belarusian':'Belarusian',
    'bulgarian':'Bulgarian',
    'czech':'Czech',
    'danish':'Danish',
    'dutch':'Dutch',
    'english':'English',
    'finnish':'Finnish',
    'french':'French',
    'german':'German',
    'germannewspelling':'German',
    'hungarian':'Hungarian',
    'indonesian':'Indonesian',
    'italian':'Italian',
    'norwegianbokmal':'Norwegian',
    'norwegiannynorsk':'Norwegian',
    'polish':'Polish',
    'portuguesestandard':'Portuguese',
    'russian':'Russian',
    'serbiancyrillic':'Serbian',
    'spanishmodernsort':'Spanish',
    'spanishtraditionalsort':'Spanish',
    'swahili':'Swahili',
    'swedish':'Swedish',
    'ukrainian':'Ukrainian'}

ShortLanguages = { #short versions of language names
    'afrikaans':'afr',
    'basque':'baq',
    'belarusian':'bel',
    'bulgarian':'bul',
    'czech':'cze',
    'danish':'dan',
    'dutch':'dut',
    'english':'eng',
    'finnish':'fin',
    'french':'fra',
    'german':'ger',
    'germannewspelling':'ger',
    'hungarian':'hun',
    'indonesian':'ind',
    'italian':'ita',
    'norwegianbokmal':'nob',
    'norwegiannynorsk':'nno',
    'polish':'pol',
    'portuguesestandard':'por',
    'russian':'rus',
    'serbiancyrillic':'scc',
    'spanishmodernsort':'spa',
    'spanishtraditionalsort':'spa',
    'swahili':'swa',
    'swedish':'swe',
    'ukrainian':'ukr'}

SF = '(?<![\\\])' #backslash filter
Tags = { #key:replacement ('old tag':'new tag')
    '[b]':'<b>','[/b]':'</b>',                          #bold
    '[i]':'<i>','[/i]':'</i>',                          #italic
    '[u]':'<u>','[/u]':'</u>',                          #underline
    '[*]':'','[/*]':'',                                 #secondary representation
    '[trn]':'<dtrn>','[/trn]':'</dtrn>',                #translate zone (direct meaning)
    '[t]':'<transcription>','[/t]':'</transcription>',  #transcription
    '[c]':'<color code="black">','[/c]':'</color>',                  #coloured text. If [c] with not specified color - then use default color.
    '[m]':'',
    '[/m]':'',                                          #end of indentation
    '[ex]':'<example>','[/ex]':'</example>',            #example
    '[ref]':'<kref>','[/ref]':'</kref>',                #reference to an another article
    '[url]':'<iref>','[/url]':'</iref>',
    '[!trs]':'','[/!trs]':'',                           #exclude from indexation
    '[/lang]':'',                                       #end of language code, if it different from translate language
    '[p]':'<abr>','[/p]':'</abr>',                      #show abbreviation
    '[sub]':'<sub>','[/sub]':'</sub>',                  #low index
    '[sup]':'<sup>','[/sup]':'</sup>',                  #high index
    '[com]':'<comment>','[/com]':'</comment>',          #comments zone
    '{{':'<!--','}}':'-->',                             #absolutely ignored comments
    '<':'&lt;','>':'&gt;',
    '&':'&amp;',
    '"':'&quot;',
	'\r\n':'\n'}

TagsPattern = '\s+(?:\[\*\])*@\s*.*|\s+(?:\[/\*\])*@\s*$|\[b\]|\[/b\]|\[i\]|\[/i\]|\[u\]|\[/u\]|\[\*\]|\[/\*\]|\[trn\]'+\
    '|\[/trn\]|\[t\]|\[/t\]|\[c\s+[a-zA-Z]+\s*\]|\[c\]|\[/c\]|\[m\]|\[m\d\]|\[/m\]|\[ex\]'+\
    '|\[/ex\]|\[\!trs\]|\[/\!trs\]|'+SF+'<<.*?'+SF+'>>|\[\s*lang\s+id\s*\=\s*\d\s*\]'+\
    '|\[/lang\]|\[p\]|\[/p\]|\[sub\]|\[/sub\]|\[sup\]|\[/sup\]|\[com\]'+\
    '|\[/com\]|\[ref\]|\[/ref\]|\{\{|\}\}|'+SF+'\^'+SF+'~|'+SF+'~|\<|\>|\[s\].+?\[/s\]|\[url\]|\[/url\]|&|"|\r\n'

XMLHeaderData = {  #Copirights, descriptions, versions and other stuff
    'type'                      : '',
    'lang_from'                 : '',
    'lang_to'                   : '',
    'description'               : '',
    'full_name'					: ''}

def perror ( *message ) :
    """
    Prints messages to stderr.
    Accepts variable number of arguments.
    """
    for a in message : print >> sys.stderr, a,

def terminate ( *msg ) :
    """
    Terminate the program with post mortem message.
    """
    perror('Error: ', *msg)
    sys.exit(1)


def ParseDSLHeader(DSLFileDesc, UTF) :
	"""DSLFileDesc - input file descriptor,
	UTF - input file encoding

	Return: Contents of the #SOURCE_CODE_PAGE tag (if found) or ''.
	"""
	global Name, FromLanguage, ToLanguage

	DSLSourceCodePage = ''

	while True :
		strng = DSLFileDesc.readline()
		if not strng :
			DSLFileDesc.close()
			break

		cd = re.search('#NAME(?:\s+)"(.+)"',strng)
		if cd :
			if not Name :
				#print 'Found tag "#NAME" - dictionary name: "'+cd.group(1)+'"'
				Name = cd.group(1)
			continue

		try :
			cd = re.search('#INDEX_LANGUAGE(?:\s+)"([a-zA-Z]+)"',strng)
			if cd :
				if not FromLanguage :
					#print 'Found tag "#INDEX_LANGUAGE" - original language: "'+cd.group(1)+'"'
					FromLanguage = cd.group(1).lower()
				continue

			cd = re.search('#CONTENTS_LANGUAGE(?:\s+)"([a-zA-Z]+)"',strng)
			if cd :
				if not ToLanguage :
					#print 'Found tag "#CONTENTS_LANGUAGE" - target language: "'+cd.group(1)+'"'
					ToLanguage = cd.group(1).lower()
				continue

			cd = re.search('#SOURCE_CODE_PAGE(?:\s+)"([a-zA-Z]+)"',strng)
			if cd :
				if not UTF : #if file is in unicode - ignore tag #SOURCE_CODE_PAGE
					#print 'Found tag "#SOURCE_CODE_PAGE" - source code page: "'+cd.group(1)+'"'
					try :
						DSLSourceCodePage = Encodings[cd.group(1).lower()]
					except KeyError : terminate('Unsupported code page: "'+cd.group(1)+'", save input file in Unicode.\n')
				continue
		except KeyError : terminate('Unknown tag value "'+cd.group(1)+'"')

#    if not strng[:1].isspace(): MainBuffer.insert(0,strng);break
		if not strng[:1].isspace(): DSLFileDesc.seek(strng.__len__()*-1,1);break
	return DSLSourceCodePage
#--------------------- end of DSL header


def RecodeToUTF8(Path, FileType) :
	"""Recodes file from utf16 to utf8,
	saves to a temporal file, and returns descriptor to it.
	FileType - 'dsl' or 'abr' or 'ann'"""

	try:

		print 'Recoding "'+FileType+'" file to UTF-8'

		Encoding = 'CP1251'
		FileLength = os.stat(Path)[ST_SIZE]
		OnePercSize = FileLength/100; OP = 0 #for showing percents


		UTF, SignatureLen = DetectByteOrderMark(Path)
		FromDesc = codecs.open(Path, 'rb', UTF)
		FromDesc.read(SignatureLen) #skip UTF signature
		if not UTF and FileType != 'ann':
			EncFromHeader = ParseDSLHeader(FromDesc, UTF)
			FromDesc.seek(0)
			if EncFromHeader : Encoding = EncFromHeader

		ToDesc = tempfile.TemporaryFile()
		while True :
			buf = FromDesc.read(10240)
			if not buf : break

			OP += 10240 #show percents
			if OP >= OnePercSize: print ''+str(FromDesc.tell()/OnePercSize)+'%\r',;sys.stdout.flush(); OP = 0

			if not UTF : buf = unicode(buf, Encoding)

			ToDesc.write(buf.encode("UTF-8"))
		ToDesc.seek(0)
		ParseDSLHeader(ToDesc, UTF)
		print '100%\n'
		return ToDesc

	except IOError, msg : terminate('\nCan\'t recode file to UTF-8. Error:\n'+str(msg),'\n')
	except UnicodeError : terminate("\nNot permitted symbol in the input file. Recode the file to UTF-16.")


def print_usage (stream, exit_code):
    print >> stream, 'Abbyy Lingvo 8 DSL to XDXF conversion\n\n'\
       'Usage: '+ProgramName+' options input_file output_directory\n'
    print >> stream, \
"""
 If a parameter surrounded with brackets - it's an optional parameter.

 [-h]               This help

 [-T] Dictionary type "translation"
 [-X] Dictionary type "explanatory"
 [-E] Dictionary type "encyclopedia"
 [-S] Dictionary type "spelling"
 [-A] Dictionary type "audio"

 [-n <name>]        Dictionary name (one word)
 [-z <full_name>]   Full name of the dictionary, like it would appear on
                      the book cover. It may contain non-English symbols.
 [-d <descr>]       Short description
 [-f <from>]        "From" language
 [-t <to>]          "To" language
 [-a <file>]        Abbreviations file (if autodetect doesn't work)
 [-i]               Put visual indentation to the output file for human reading.

 Example:

 $ """ + ProgramName + """ -n General -E -d "My dictionary"\
-t English orig_dict.dsl /var/dicts"""

    sys.exit(exit_code)



try : optlist,args = getopt.getopt (sys.argv[1:], 'hTXESAn:z:d:f:t:a:i')
except getopt.GetoptError, msg: perror('\n'+str(msg)+'\n'); print_usage(sys.stderr, 1)

if not optlist and not args :
    print_usage(sys.stderr, 1)
    sys.exit(1)

for option, arg in optlist :

    if option == '-T':      # dictionary type - 'translation'
        if XMLHeaderData['type'] : XMLHeaderData['type'] += ', '
        XMLHeaderData['type'] += 'translation'

    elif option == '-X':      # dictionary type - 'explanatory'
        if XMLHeaderData['type'] : XMLHeaderData['type'] += ', '
        XMLHeaderData['type'] += 'explanatory'

    elif option == '-E':      # dictionary type - 'encyclopedia'
        if XMLHeaderData['type'] : XMLHeaderData['type'] += ', '
        XMLHeaderData['type'] += 'encyclopedia'

    elif option == '-S':      # dictionary type - 'spelling'
        if XMLHeaderData['type'] : XMLHeaderData['type'] += ', '
        XMLHeaderData['type'] += 'spelling'

    elif option == '-A':      # dictionary type - 'audio'
        if XMLHeaderData['type'] : XMLHeaderData['type'] += ', '
        XMLHeaderData['type'] += 'audio'

    elif option == '-n':      # -n dictionary name
        Name = arg      # if not specified, then takes from DSL: NAME

    elif option == '-z':      # -z full dictionary name
        XMLHeaderData['full_name'] = unicode(arg, getdefaultlocale()[1]).encode("UTF-8")

    elif option == '-d':    # -d description
        XMLHeaderData['description'] = unicode(arg, getdefaultlocale()[1]).encode("UTF-8")

    elif option == '-f':    # -f from language
        FromLanguage = arg  # if not specified, then takes from DSL: INDEX_LANGUAGE

    elif option == '-t':    # -t to language
        ToLanguage = arg    # if not specified, then takes from DSL: CONTENTS_LANGUAGE

    elif option == '-a':    # -a path to the abbreviations file
        AbbrevFile = arg


    elif option == '-i':    # -i put visual indentation
		NL = '\n'
		TB = '\t'

    elif option == '-h':    # -h help
        print_usage(sys.stdout, 0)

    else : perror('Unrecognized option: '+option);sys.exit (1)  #Something else: unexpected.


if not XMLHeaderData['type'] :
    perror('You have to specify one, or few options - "Dictionary type".\n')
    sys.exit(1)

if args.__len__() > 2:
    perror('\nToo many non option arguments.\n\n')
    print_usage (sys.stderr, 1)

if args.__len__() < 1:
    perror('\nToo few non option arguments.\n\n')
    print_usage (sys.stderr, 1)

if not os.path.exists(args[0]):
    perror(args[0], ' - not such file, or not a file.')
    sys.exit (1)

#change current directory
try :
    os.chdir(os.path.dirname(args[0]) or os.curdir)
except OSError, msg : perror('Cant\'t change directory. Error: '+str(msg))

def DetectByteOrderMark(filename):
    """
    Opens and tests UTF file encoding:
    BOM_UTF8
    BOM_UTF16_BE
    BOM_UTF16_LE
    BOM_UTF32_BE
    BOM_UTF32_LE

    Returns: encoding, signature length
    If no signature found returns: None, 0
    """
    encodings = [ ( codecs.BOM_UTF32,   'utf-32',   4 ),
        ( codecs.BOM_UTF32_BE,  'utf-32-be',4 ),
        ( codecs.BOM_UTF32_LE,  'utf-32-le',4 ),
        ( codecs.BOM_UTF16_BE,  'utf-16-be',2 ),
        ( codecs.BOM_UTF16_LE,  'utf-16-le',2 ),
        ( codecs.BOM_UTF8,      'utf-8',    3 ) ]
    try :
        if os.path.isfile(filename) :
            f = file(filename,'rb')
            header = f.read(4) # Read just the first four bytes.
            f.close()
        else :
            perror('DetectByteOrderMark() - There\'s no file name:'+filename)
            sys.exit(1)
    except IOError, msg:
        perror('Can\'t test encoding of the file. Error:\n'+str(msg))
        sys.exit(1)

    for h,e,l in encodings :
        if header.find(h) == 0:
            return e,l
    return None, 0


try :
	#search and import annotation file
	for ann in os.listdir(os.getcwd()):
		if ann.lower() == os.path.splitext(ann.lower())[0]+'.ann' :
			print '\nFound annotation file: '+ann
			AnnFileDesc = RecodeToUTF8(ann,'ann')

			if XMLHeaderData['description'] :
				print 'Combining an annotation with a description, specified from command line.'
				XMLHeaderData['description'] += '\n\n'

			while True :
				strng = AnnFileDesc.readline()
				if not strng :
					AnnFileDesc.close()
					break
				XMLHeaderData['description'] += strng.strip()+'\n'

			XMLHeaderData['description'] += '\n\nThis dictionary was converted from Lingvo DSL format.\n'\
				'Converter script: '+ProgramName+' v.'+Version+' by Alexander Goryachev\nthorn_st@users.sourceforge.net'
			break
except IOError, msg : terminate("Problem with an annotation file: "+str(msg))




#open DSL file
DSLFileDesc = RecodeToUTF8(args[0],'dsl')


#--------------------- Writing XDXF dictionary header ---------------------
print 'Creating XDXF dictionary header'

try :
    if FromLanguage :
        XMLHeaderData['lang_from'] = ShortLanguages[FromLanguage.lower()]
        FromLanguage = LongLanguages[FromLanguage.lower()]
except KeyError :
    terminate('Unknown language: ', FromLanguage, '\nPossible values are:\n',LongLanguages.keys(),'\n')

try :
    if ToLanguage :
        XMLHeaderData['lang_to'] = ShortLanguages[ToLanguage.lower()]
        ToLanguage = LongLanguages[ToLanguage.lower()]
except KeyError :
    terminate('Unknown language: ', ToLanguage, '\nPossible values are:\n',LongLanguages.keys(),'\n')


if not Name : terminate('Can\'t find a dictionary name - specify it manually by -n option')
if not FromLanguage : terminate('Can\'t find a source language name - specify it manually by -f option')
if not ToLanguage : terminate('Can\'t find a target language name - specify it manually by -t option')
if not XMLHeaderData['description'] : XMLHeaderData['description'] = 'Dictionary "'+Name+'", imported from DSL format Lingvo'

Name = Name.lower()
if args.__len__() == 1:
    OutputDir = os.path.join(os.curdir, Name) #if not specified output directory, then use current
else : OutputDir = os.path.join(args[1], Name)

#create output directory
if not os.path.exists(OutputDir) :
    while True :
        yn = raw_input('Path "'+os.path.abspath(OutputDir)+'" doesn\'t exists. Create? (y/n): ').lower().strip()
        if yn == 'y' : os.makedirs(OutputDir); break
        elif yn == 'n' : print 'Cancelled by user.'; sys.exit(0)
        else : print "Enter only one symbol 'Y' or 'N' please."


OutFileName = os.path.join(OutputDir, 'dict.xdxf').lower()

#open an output xdxf file
try:
    outfile = file(OutFileName, 'wb')
except IOError, msg:
    perror('Cannot open output file: "'+OutFileName+'"\nError: '+str(msg))
    sys.exit (1);

print '\n'+'-'*40
print 'Source DSL-file: %s'			% args[0]
print 'Output directory: %s'		% os.path.abspath(OutputDir)
print 'Dictionary name: %s'			% Name
print 'Full dictionary name: %s'	% XMLHeaderData['full_name']
#print 'Description: %s'         % '\n'+'-'*20+'\n'+re.sub('<br />','\n',XMLHeaderData['description'])+'-'*20+'\n'
print 'Source Language: %s'			% FromLanguage
print 'Target Language: %s'			% ToLanguage
print 'Dictionary type: %s'			% XMLHeaderData['type']
#print 'Transcription: %s'       % Trans
print '-'*40, '\n'

try:
    outfile.write('<?xml version="1.0" encoding="UTF-8" ?>'+NL+
        '<xdxf>'+NL+
        '<info>'+NL)

    outfile.write(
        TB+'<full_name>'+XMLHeaderData['full_name']+'</full_name>'+NL+
        TB+'<lang_from>'+XMLHeaderData['lang_from']+'</lang_from>'+NL+
        TB+'<lang_to>'+XMLHeaderData['lang_to']+'</lang_to>'+NL+
        TB+'<type>'+XMLHeaderData['type']+'</type>'+NL+
        TB+'<description>'+XMLHeaderData['description']+'</description>'+NL+
        '</info>'+NL)

except IOError, msg : terminate("Can't write to the output file: "+str(msg))
#--------------------- end of XDXF header
Key = u''
SubArticle = False

def Replacement (matchobj):
	"""Tag replacer for re.sub"""

	global SubArticle

	#Subarticles
	subartbegin = re.search('\s+(?:\[\*\])*@\s*(.*)',matchobj.group(0))
	if subartbegin :
        #print 'found subarticle'+subartbegin.group(1)
		if SubArticle :
			if  subartbegin.group(1).__len__(): return '</def>'+NL+TB+TB+'</subarticle>'+NL+TB+TB+'<subarticle>'+NL+TB+TB+'<key>'+subartbegin.group(1).strip()+'</key>'+NL+TB+TB+'<def>'
			#else : return '</def>\n\t\t</subarticle>\n\t\t<subarticle>\n\t\t<def>'
			else :
				SubArticle = False
				return '</def>'+NL+TB+TB+'</subarticle>'
		SubArticle = True
		return '<subarticle>'+NL+TB+TB+'<key>'+subartbegin.group(1).strip()+'</key>'+NL+TB+TB+'<def>'+NL

	#reference to another key-phrase
	reference =re.search('<<(.*?)>>',matchobj.group(0))
	if reference : return '<kref>'+reference.group(1)+'</kref>'

	#indentation
	indent =re.search('\[m(\d)\]',matchobj.group(0))
	if indent : return ' '*int(indent.group(1))
	#if indent : return ' '

	#colourings
	color = re.search('\[c\s+([a-zA-Z]+)\s*\]',matchobj.group(0))
	if color : return '<color code="'+color.group(1).lower()+'">'


	FileLink = re.search('\[s\](.+?)\[/s\]',matchobj.group(0))
	if FileLink :
		print 'Found reference to a file: '+FileLink.group(1)
		try :
			shutil.copyfile(FileLink.group(1), os.path.join(os.path.dirname(OutFileName), FileLink.group(1)))
		except IOError, msg : terminate("File link wasn't processed: "+str(msg))


		return '<rref>'+os.path.basename(FileLink.group(1).lower())+'</rref>'

	LangID = re.search('\[\s*lang\s+id\s*\=\s*\d\s*\]',matchobj.group(0))
	if LangID : return ''

	RoofTilda = re.search('\^~',matchobj.group(0))
	if RoofTilda :
		return Key[:1].swapcase()+Key[1:].rstrip()

	Tilda = re.search('~',matchobj.group(0))
	if Tilda :
		return Key.rstrip()

	#all other tags
	return Tags[matchobj.group(0)]

def KillSlashes (matchobj):
    #double slashes
	DoubleSlashes =re.search('\\\\\\\\',matchobj.group(0))
	if DoubleSlashes : return '\\'

	#slashes
	Slash =re.search('\\\\',matchobj.group(0))
	if Slash : return ''


#--------------------- Process abbreviations ---------------------------
try :

	AbbrevFileDesc = 0
	if AbbrevFile:
		FileLength = os.stat(AbbrevFile)[ST_SIZE]
		AbbrevFileDesc = RecodeToUTF8(AbbrevFile,'abr')
	else:
		for AbbrevFile in os.listdir(os.getcwd()):
			if 'abbrev.dsl' in AbbrevFile.lower() or 'abrv.dsl' in AbbrevFile.lower() :
				while True :
					yn = raw_input('Found an abbreviations file: "'+AbbrevFile+'" Import? (y/n): ').lower().strip()
					if yn == 'y' :
						FileLength = os.stat(AbbrevFile)[ST_SIZE]
						AbbrevFileDesc = RecodeToUTF8(AbbrevFile,'abr')
						break
					elif yn == 'n' : break
					else : print "Enter only one symbol 'Y' or 'N' please."
				if AbbrevFileDesc : break


	if AbbrevFileDesc :
		print 'Processing abbreviations'

		ArticleBegin = False

		Begin = True #begin of a file, before of any articles
		outfile.write('<abbreviations>'+NL)

		OnePercSize = FileLength/100; OP = 0 #for showing percents

		while True :
			strng = AbbrevFileDesc.readline()
			if not strng :
				AbbrevFileDesc.close()
				break

			OP += strng.__len__() #show percents
			if OP >= OnePercSize: print ''+str(AbbrevFileDesc.tell()/OnePercSize)+'%\r',;sys.stdout.flush(); OP = 0

			if not strng : continue

			if not strng[:1].isspace() and not ArticleBegin and not Begin:
				outfile.seek(-1,1)
				outfile.write('</val>'+NL+TB+'</abr_def>'+NL)

			if not strng[:1].isspace() and not ArticleBegin:
				ArticleBegin = True
				outfile.write(TB+'<abr_def>'+NL)
				Begin = False
				ValBegin = True

			if  not strng[:1].isspace() :
				outfile.write(TB+'<key>'+re.sub('(\\\\\\\\)|(\\\\)',KillSlashes,strng).strip()+'</key>'+NL)
				continue

			ArticleBegin = False

			if ValBegin == True : outfile.write(TB+TB+'<val>'); ValBegin = False

			strng = strng.strip()
			if strng : outfile.write(re.sub('(\\\\\\\\)|(\\\\)',KillSlashes,re.sub(TagsPattern,Replacement,strng))+'\n')
		outfile.seek(-1,1)
		outfile.write('</val>'+NL+TB+'</abr_def>'+NL)
		outfile.write('</abbreviations>'+NL)
		print '100%\n'
except IOError, msg : terminate("Problem with an abbreviation file: "+str(msg))

#--------------------- End of process abbreviations ---------------------------

#--------------------- Process dictionary body ---------------------------
print 'Processing dictionary body'


def FindInKeys (matchobj):
    """Tag replacer for re.sub"""

    #round brackets
    RoundBrackets =re.search('\((.*?)\)',matchobj.group(0))
    if RoundBrackets : return '<opt>'+RoundBrackets.group(1)+'</opt>'

    #curly brackets
    CurlyBrackets =re.search('\{(.*?)\}',matchobj.group(0))
    if CurlyBrackets : return CurlyBrackets.group(1)

Article = 0 #Articles counter
ArticleBegin = False
BufferCounter = 0

DefOpen = False #temporal crutch for tag <def>

#FleLength = os.stat(DSLFileDesc)[ST_SIZE]
SavePos = DSLFileDesc.tell()
DSLFileDesc.seek(0,2)
FileLength = DSLFileDesc.tell() - SavePos
DSLFileDesc.seek(SavePos)

OnePercSize = FileLength/100; OP = 0 #for showing percents



try :
	while  True :
		strng = DSLFileDesc.readline()
		if not strng :
			DSLFileDesc.close()
			break


		OP += strng.__len__() #show percents
		if OP >= OnePercSize: print ''+str(DSLFileDesc.tell()/OnePercSize)+'%\r',;sys.stdout.flush(); OP = 0

		if not strng : continue


		if not strng[:1].isspace() and not ArticleBegin and Article:
			DefOpen = False; outfile.write(TB+'</def>'+NL)#temporal crutch for tag <def>
			outfile.write('</article>'+NL)

		if not strng[:1].isspace() and not ArticleBegin:
			Key = strng
			ArticleBegin = True
			Article += 1
			outfile.write('<article>'+NL)

		if not strng[:1].isspace() :
			outfile.write(TB+'<key>'+re.sub('(\\\\\\\\)|(\\\\)',KillSlashes,re.sub(SF+'\((.*?)'+SF+'\)|'+SF+'\{(.*?)'+SF+'\}',FindInKeys,strng)).strip()+'</key>'+NL)
			continue

		ArticleBegin = False

		#temporal crutch for tag <def>
		if not DefOpen : outfile.write(TB+'<def>'+NL); DefOpen = True

		#strng = TB+TB+re.sub(TagsPattern,Replacement,strng.strip())
		strng = TB+TB+re.sub(TagsPattern,Replacement,strng)
		#if strng[-1:] == '>' : outfile.write(strng+NL)
		#else : outfile.write(strng+'\n'+NL)

		outfile.write(re.sub('(\\\\\\\\)|(\\\\)',KillSlashes,strng)+NL)
		#outfile.write(strng+NL)
	#--------------------- end of dictionary body

	outfile.write(TB+'</def>'+NL)#temporal crutch for tag <def>

	outfile.write('</article>'+NL)
	outfile.write('</xdxf>')
	print '100%\n'


except IOError, msg : terminate("Can't write to the output file: "+str(msg))

outfile.close()

#search and copy dictionary icon file
for pic in os.listdir(os.getcwd()):
	if pic.lower() == os.path.splitext(pic.lower())[0]+'.bmp' :
		print '\nFound an icon file: '+pic
#copy icon to the output directory with the name of the base dictionary and "bmp" extention
		try :
			shutil.copyfile(pic, os.path.splitext(OutFileName)[0]+'.bmp')
		except IOError, msg : terminate("The dictionary icon wasn't copied: "+str(msg))
		print 'The dictionary icon file copied as: "'+os.path.basename(os.path.splitext(OutFileName)[0]+'.bmp')+'"'
		break


print '\n\n***SUCCESS***\n'
