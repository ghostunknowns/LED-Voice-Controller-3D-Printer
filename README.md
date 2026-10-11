# Bambu P1S LED & Voice Controller

Image of pcb here

<img src="/readme-assets/pcb.jpeg" width="30%">



## Introduction

Hi and welcome! This project originally started as a simple way to provide better lighting for my Bambu Lab P1S 3D printer. Adding an LED strip is a popular mod, so I wanted to put my own twist on it. As an electrical engineering student, I also saw it as a great opportunity to get a head start and gain hands-on experience in PCB design and manufacturing. 

The core of the PCB is built around an ESP32 architecture. Because of this, you can actually upload the code to a standard ESP32 devkit board to test the LED portion out before the custom board is manufactured.

**Fun fact:** I didn't originally plan for voice commands. The microphone was initially meant to capture audio of the printer's telemetry instead of connecting directly to the network, but that approach didn't make it past testing. After some research, I discovered existing mods that created extra wireless screens for the printer. That's when it clicked, the same data and commands accessible through Bambu Studio can also be handled using an ESP32.

## Goals & Objectives

**Main Goal:**
Successfully design a custom PCB small enough to fit inside the slot on the 3D printer, power the connected LED strip from the printer's USB Type-A port, run custom animations based on the printer's state, and use the onboard microphone for hands-free voice control.

### Current Objectives:
1. **Design the PCB schematic and fabricate it.** *(Complete)*
2. **Test the custom PCB and LED strip with custom animations.** *(Complete)*
3. **Establish communication between the PCB and the printer.** *(Complete)*
4. **Combine the communication code with the custom LED animations.** *(Complete)*
5. **Test if the microphone can receive voice inputs clearly.** *(In Progress)*
6. **Convert voice commands into actionable data the PCB can read and decipher.** *(Not Started)*

## Required Hardware
provide the pcb design images, maybe files

<img src="readme-assets/schematic-editor.png" width="50%">
<img src="readme-assets/pcb-editor.png" width="50%">

### Powering 
how does the pcb get power from the printer

<img src="readme-assets/pcb_in_printer.jpeg" width="50%">

## Features
list the capabilites

### Startup&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;Loading Filament&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp; Unloading Filament
<img src="readme-assets/startup.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/loading_filament.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/unloading_filament.webp" width="25%">

### Printing&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;Standby&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp; Print Finished
<img src="readme-assets/printing.jpeg" width="33%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/standby.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/print_finish.webp" width="25%">


## Installation
briefly explain how the code aspect works

explain what's needed to run the code
