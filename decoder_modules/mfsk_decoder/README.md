# mfsk_decoder

Module SDR++ pour décoder en direct la famille des modes **MFSK** de FLDIGI.
Maintenu par **F4JTV** (ADRASEC 06).

Interface et fonctionnement calqués sur le module `psk_decoder` : choix de la
bande latérale (USB / LSB / NFM), réglage de la fréquence audio (AF), squelch de
puissance, et zone de texte décodé.

Le back-end de décodage est un **portage fidèle du récepteur MFSK de FLDIGI**
(classes Viterbi et entrelaceur reprises verbatim, Varicode IZ8BLY identique,
même mathématique de réception). Il est **validé sur un signal réel** : un
enregistrement MFSK16 du commerce décode correctement (voir « Validation »).

## Modes implémentés (les 11 de FLDIGI)

| Mode      | Tons | bits/symbole | baud   | Fe natif | symlen | Entrelaceur (profondeur) |
|-----------|-----:|-------------:|-------:|---------:|-------:|-------------------------:|
| MFSK4     | 32   | 5            | 3,906  | 8000     | 2048   | 5                        |
| MFSK8     | 32   | 5            | 7,8125 | 8000     | 1024   | 5                        |
| MFSK11    | 16   | 4            | 10,766 | 11025    | 1024   | 10                       |
| MFSK16    | 16   | 4            | 15,625 | 8000     | 512    | 10                       |
| MFSK22    | 16   | 4            | 21,533 | 11025    | 512    | 10                       |
| MFSK31    | **8**| 3            | 31,25  | 8000     | 256    | 10                       |
| MFSK32    | 16   | 4            | 31,25  | 8000     | 256    | 10                       |
| MFSK64    | 16   | 4            | 62,5   | 8000     | 128    | 10                       |
| MFSK128   | 16   | 4            | 125    | 8000     | 64     | 20                       |
| MFSK64L   | 16   | 4            | 62,5   | 8000     | 128    | **400**                  |
| MFSK128L  | 16   | 4            | 125    | 8000     | 64     | **800**                  |

L'espacement des tons vaut `Fe / symlen` (= baud). Les variantes « L » utilisent
le même schéma avec un entrelaceur beaucoup plus profond (meilleure résistance
au fading, latence plus longue). Tous les paramètres proviennent de la table de
modes de FLDIGI (`src/mfsk/mfsk.cxx`).

## Chaîne de traitement

```
IQ du VFO @48 kHz
  -> PowerSquelch (puissance)
  -> démodulation USB/LSB (SSB) ou NFM (FM)  -> audio réel @48 kHz
  -> banc de N corrélateurs (un par ton, à AF + k x espacement)
  -> synchro symbole sur le PIC de corrélation (16 sous-phases)
  -> [back-end FLDIGI, verbatim]
       graydecode(i) = i ^ (i>>1)
       décision souple (b[k] += +/- |bin|, le ton gagnant vote x2)
       désentrelaceur en CASCADE (depth etages symbits x symbits, sens REV)
       DOUBLE Viterbi souple R=1/2 K=7 (0x6d / 0x4f), arbitrage par metrique
       Varicode IZ8BLY (cadrage sur (reg & 7) == 1) -> ASCII
```

Le corrélateur tourne directement au débit du VFO (48 kHz) : la fenêtre fait
`round(48000 / baud)` echantillons et le back-end FLDIGI est indépendant du
débit d'echantillonnage. La synchro symbole se cale sur le maximum du pic de
corrélation (atteint quand la fenetre est centree sur un symbole), ce qui reste
robuste sur porteuse continue, contrairement a un calage sur l'energie totale.

**Fréquence AF** = fréquence du **ton le plus bas** (bord gauche du signal sur le
waterfall), conformément à la convention de curseur de FLDIGI. On règle l'AF sur
le bord gauche de la trace MFSK.

## AFC — calage automatique de l'AF

Une case **AFC** (activée par défaut) cale automatiquement la fréquence audio sur
le signal présent : plus besoin de pointer précisément le bord de la trace.

Principe : le module garde un buffer glissant de quelques secondes d'audio et
balaie la bande SSB en deux étages — un repère grossier (spectre sur la grille de
tons, par Goertzel) puis un affinage fin par corrélation. Le candidat retenu
maximise une métrique **confiance × entropie d'occupation des tons** : un vrai
signal MFSK visite tous ses tons (entropie élevée) avec un pic net, ce qui le
distingue d'une porteuse pure, d'un RSID/TX-ID ou du bruit. En cas d'égalité, le
bord gauche le plus bas est préféré (= ton 0).

L'acquisition est **robuste en réception réelle** :
- un **seuil de qualité** empêche tout verrouillage sur du bruit (le balayage
  attend qu'un vrai signal apparaisse) ;
- une **ré-acquisition « meilleur score »** : tant qu'aucun verrou solide n'est
  tenu, un nouveau scan ne remplace le précédent que s'il est meilleur — un
  mauvais accrochage initial (bruit au démarrage, transitoire d'accord, signal
  capté en cours) est donc automatiquement remplacé dès qu'une fenêtre propre
  apparaît ;
- une fois fermement verrouillé, les balayages large bande s'arrêtent : le coût
  CPU en régime établi est celui d'un simple décodage.

Le balayage d'acquisition s'exécute dans un **thread de fond** : le thread audio
ne fait qu'une copie rapide du buffer puis applique le résultat quand il est
prêt. Le calcul lourd ne bloque donc jamais le flux audio ni le waterfall (le
balayage a aussi été optimisé pour rester bien sous le budget temps réel).

**Rejeu du début (« replay on lock »).** Tant que l'AFC n'a pas verrouillé, l'audio
reçu est conservé dans un buffer (jusqu'à 30 s) au lieu d'être décodé à une
fréquence encore fausse. Au moment du verrouillage, ce buffer est **rejoué depuis
le début** à travers le décodeur à la bonne fréquence : le début de la
transmission, reçu pendant les quelques secondes de recherche, n'est donc plus
perdu. Le rejeu est étalé sur plusieurs blocs audio (rattrapage ~2× temps réel)
pour ne pas provoquer d'à-coup. Si aucun verrou n'est trouvé au bout de 30 s, le
module bascule en décodage direct pour ne pas rester muet.

Validé sur enregistrements réels (RF directe incluse) depuis n'importe quelle
position de départ (700–2400 Hz), y compris avec bruit/silence avant le signal,
coupure puis reprise, ou démarrage en plein milieu de trame.

Quand l'AFC est active, le curseur AF manuel est grisé (le modem possède l'AF) et
la fréquence suivie est affichée. Désactiver l'AFC rend le réglage manuel.

## Vue de bande (repère visuel pour l'AF)

Au-dessus du curseur AF, le menu affiche un **petit spectre** de la bande audio
(300–2700 Hz) rafraîchi en continu : on y voit la trace MFSK. Un **trait jaune**
marque la position du ton 0 (= la fréquence AF) et une **zone bleutée** couvre la
largeur des tons. Pour caler manuellement (modes MFSK4/8, ou si l'AFC est
désactivée), il suffit de **cliquer / glisser dans ce spectre** pour amener le
trait jaune sur le bord gauche de la trace — plus besoin de viser à l'aveugle
avec le seul curseur. En AFC automatique, le trait suit la fréquence trouvée.

Limite : **MFSK4 et MFSK8** (32 tons espacés de ~4/8 Hz) ne sont pas résolus à la
fenêtre de corrélation ; leur calage automatique est ambigu, donc ces deux modes
gardent un réglage **manuel** de l'AF (comme FLDIGI, qui demande un placement
précis du curseur). Le décodage, lui, fonctionne pour les 11 modes.

## Compilation

Depuis la racine de SDRPlusPlus :

```bash
# 1) ajouter les lignes de root_CMakeLists_additions.txt au CMakeLists.txt racine
# 2) configurer + compiler
cd build
cmake .. -DOPT_BUILD_MFSK_DECODER=ON
make mfsk_decoder -j$(nproc)
```

Le binaire produit : `build/decoder_modules/mfsk_decoder/mfsk_decoder.so`.
Le copier dans le répertoire des modules SDR++, puis l'ajouter via
**Module Manager** (le module crée un VFO et démarre tout seul).

## Validation du décodage

1. **Signaux réels (preuve d'interopérabilité).** Deux enregistrements MFSK16
   réels sont décodés correctement par le module, y compris **en réception RF
   directe** (USB) avec le TX-ID/RSID de FLDIGI en tête de trame :
   `CQ CQ CQ de F4JTV F4JTV F4JTV  CQ CQ CQ de F4JTV F4JTV F4JTV pse k`
   (quelques caractères en tête = acquisition de la synchro avant verrouillage).
   Avec l'AFC, le décodage réussit depuis n'importe quelle AF de départ.

2. **Aller-retour FLDIGI (les 11 modes).** Un émetteur reprenant la chaîne TX
   exacte de FLDIGI (encodeur convolutif + entrelaceur FWD + grayencode +
   Varicode IZ8BLY) génère le signal, décodé ensuite par le module. **Les 11
   modes récupèrent le corps du message à 100 %** (acquisition automatique pour
   les 9 modes à tons résolus, calage manuel pour MFSK4/8).

Sûreté mémoire vérifiée (AddressSanitizer + UBSan, aucun défaut, acquisition AFC
comprise) et chargement du module / rendu du menu testés sans crash.

## Notes d'implémentation

- Le back-end (Viterbi, entrelaceur, Varicode) est repris **verbatim** de FLDIGI
  (master, W1HKJ / d'après gMFSK de OH2BNS), d'où la compatibilité bit-exacte.
- Pour les symbits **impairs** (MFSK31 = 3 bits, MFSK4/8 = 5 bits), FLDIGI lance
  **deux décodeurs Viterbi** aux deux phases d'appariement possibles et conserve
  celui dont la métrique moyenne est la meilleure ; ce comportement est reproduit.
- LSB : la géométrie du VFO bascule en bande latérale inférieure ; si un signal
  apparaît en ordre de tons inversé, l'option « reverse » de FLDIGI serait à
  ajouter (non nécessaire en USB, cas validé).
