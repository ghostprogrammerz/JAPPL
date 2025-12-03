# JAPPL
website: https://seancathancabral-bit.github.io/JAPPL/
JAPPL Scratch Project: https://scratch.mit.edu/projects/1084888676/
JAPPL Discussion Thread and docuentation : https://scratch.mit.edu/discuss/topic/837372/

JAPPL is a programming language thats significantly under developed bc its just one guy
JAPPL — A Syntax-Tolerant Text Programming Language

JAPPL (Just Another Programming PLatform/Language) is a lightweight, beginner-friendly programming language implemented inside Scratch but designed as an independent text language.
It uses its own compiler, execution engine, memory model, and syntax system.

JAPPL acts as a bridge between Scratch (easy), Python (clean), and Java (structured), allowing new programmers to write real text code with minimal frustration.

the current prototype is implemented on top of scratch using it as a host virtual machine
a standalonh html/js is planned

you are requested to help jappl grow by making a better version of jappl and share it on scratch if the update is good and found by the developer the new feature will 100% be implemented

Key Features
Syntax-tolerant

JAPPL accepts multiple variations of the same command and converts them into clean internal bytecode:

'  >print hi  '

'>print "hi"'

'  >print=hi  ' 


All normalize into:

'  >print "hi"  '


This makes JAPPL extremely easy for beginners and hard to break.

 Text-based language inside a VM

Even though JAPPL is implemented in Scratch, it is NOT a block-language.
It has:

its own tokenizer

its own compiler

its own bytecode list

its own executor

its own variable RAM

operator engine

sensing system

loops & repeat

custom memory handling

Scratch is used only as the host (like Python being hosted by C).

 Beginner-first design

Built so new programmers can learn coding without syntax nightmares:

plain-English commands

forgiving structure

instant output

consistent behaviour

portable project file

simple mental model

Core Commands (examples)
 
'  >print hello  '

'  >wait 1  '

'  >clear screen  '

' >make variable x  '

'  >set variable x to 4  '

'  >repeat infinite  '

'  >repeat 10  '

 Current Version

JAPPL 4.0 (Stable branch under development)
New features include:

rebuilt operator engine

accurate expression handling

optimized VM (runs faster and cleaner)

sensing variables (mouse, time, date)

loops: repeat + forever

stable compiler with zero known bugs

syntax cleaner / auto-normalizer

 Development Timeline (Short Summary)
JAPPL 1 — Core Engine

interpreter

text engine

compiler

basic screen commands

first public release

JAPPL 2 — Variables & Operators

make/set/delete variable

ask (input)

operator system

calculator logic (NTPrograms credit)

GUI improvements

JAPPL 3 — Real Language Features

operator compatibility

multiple-variable expressions

better debugging logs

sensing variables

repeat/forever loops

stable memory system

JAPPL 4 — Stability & Architecture

rebuilt parser

rebuilt operator evaluator

fixed token duplication bugs

optimized executor

polished syntax normalizer

Planned Features

if / conditional logic

multi-script support (JAPPL 8–10)

GUI redesign

tabs for multiple sprites and their scripts

typing-based script editor

Contribution

You are welcome to:

suggest features

open issues

remix JAPPL

build new commands

improve the engine

submit pull requests

All feedback and remixes are very very appreciated.
The official JAPPL maintainers reserve final approval on what becomes part of JAPPL.
contributions must be remixed from the main project (

 License (Apache 2.0)

You are free to:

remix

modify

improve

use in personal/commercial projects

BUT you must:

credit JAPPL and the original author (sean)

state clearly if your version is modified

NOT claim your fork is the “official JAPPL”

You may keep your remix closed-source.

Full license text is in LICENSE.
