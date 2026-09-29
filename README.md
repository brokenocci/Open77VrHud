# Open77 VR HUD

RED4ext plugin that draws the Open77 WebUI into the headset while playing
Cyberpunk 2077 with R.E.A.L. VR and Virtual Desktop.

Plugin RED4ext che disegna la WebUI di Open77 nel visore, con Cyberpunk 2077,
R.E.A.L. VR e Virtual Desktop.

Licenza: [MIT](LICENSE). Si può usare, copiare e modificare.

## Cosa fa

Open77 compone le pagine CEF sul monitor, dopo che REAL VR ha già mandato le
immagini al visore. Questo plugin aggancia `ovr_EndFrame` (LibOVR di Virtual
Desktop oppure quello dell'app Meta Horizon), riapre in D3D11 le texture
condivise delle pagine e le mette su un layer quad con la stessa posa della
board HUD di REAL VR.

Ogni superficie Open77 è un ring di 3 texture. Viene disegnato solo lo slot
scritto per ultimo. La schermata di connessione e il menu del freeroam vengono
nascosti quando il log di Open77 dice che sono chiusi, perché quelle pagine non
cancellano la texture.

## Requisiti

- Windows 10 o 11 a 64 bit
- Cyberpunk 2077 2.31
- RED4ext 1.30.0
- Client Open77 per la 2.31
- R.E.A.L. VR 26.3.0, già in grado di entrare in VR
- Virtual Desktop, runtime OpenXR su VDXR
- GPU NVIDIA. Provato su RTX 4070 Ti 12 GB con `ForcedRes=1232,1344` e circa
  10 pixel per grado. A risoluzioni VR più alte i 12 GB non bastano.
- 32 GB di RAM e un file di paging gestito da Windows

REAL VR, nell'installazione normale, ha già la risoluzione `3088x3088` nella
lista del gioco. Non va cambiata dal menu video: quel menu può riscrivere
`options.json` e il gioco si chiude all'avvio.

## Installazione

Con il gioco chiuso, copia `Open77VrHud.dll` compilata e `Open77VrHud.ini` in:

```
red4ext/plugins/Open77VrHud/
```

RED4ext carica il plugin da solo. Non c'è un interruttore nel launcher Open77.

## Compilazione

Serve [Zig](https://ziglang.org/) 0.14.1.

```
zig cc -shared -O2 -target x86_64-windows-gnu -o Open77VrHud.dll Open77VrHud.c -ld3d12 -ld3d11 -ldxgi -lole32 -luuid -lpsapi
```

## Stato

Il sorgente pubblicato è la 4.5.0. La DLL in uso su una macchina di sviluppo
può essere una 4.6.0 compilata a parte: il sorgente di quella build non è in
questo albero.
