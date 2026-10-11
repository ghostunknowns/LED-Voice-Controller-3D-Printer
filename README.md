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

To build this project, you will need the custom PCB based on the designs shown below, along with the following components:

* **3D Printer:** A Bambu Lab P1S. (It may work with other Bambu Lab printers, but this has only been tested on the P1S).
* **LED Strip:** A 5V LED strip. (For reference, I used 81 LEDs cut from a 60 LEDs/meter strip).
* **Printed Enclosures:** You will need to print the custom housing pieces to mount the LED strip inside the printer.

**Note on PCB Files:** I am intentionally not sharing the raw PCB project files. Since I've provided the schematics and layout below, I encourage you to design your own board! It's a great hands-on learning experience, and it gives you the chance to put your own twist on the project! 

* **Component Sourcing:** DigiKey
* **PCB Fabrication:** JLCPCB

### Useful Links

* [LED Strip](https://www.amazon.com/dp/B0BNN1ZQHS?th=1)
* [3D Printed Parts](https://makerworld.com/en/models/99746-low-profile-p1s-x1c-p1p-riser-with-diffused-led?from=search#profileId-106353)

### Schematic

<img src="readme-assets/schematic-editor.png" width="50%">

### Layout

<img src="readme-assets/pcb-editor.png" width="50%">

### PCB in External USB Port 

<img src="readme-assets/pcb_in_printer.jpeg" width="50%">

### For Those Worried About Soldering

Don't worry, there are surface mounted components, so no hand soldering is required for this project. All you need is solder paste and a hotplate. 

When applying the solder paste, you have a few options:
* **Stencil:** You can order a stencil along with your PCB from the fabricator to make applying paste quick and precise.
* **Syringe / Manual:** You can use the syringe that usually comes with the solder paste to manually apply a small amount to each pad. 

*(For reference, I used a syringe directly, so a stencil isn't strictly necessary—though it can definitely save you some time!)*

## Features
list the capabilites

### Startup&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;Loading Filament&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp; Unloading Filament
<img src="readme-assets/startup.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/loading_filament.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/unloading_filament.webp" width="25%">

### Printing&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;Standby&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp;&emsp; Print Finished
<img src="readme-assets/printing.jpeg" width="33%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/standby.webp" width="25%">  &emsp;&emsp;&emsp;&emsp; <img src="readme-assets/print_finish.webp" width="25%">


## Installation
briefly explain how the code aspect works

explain what's needed to run the code
