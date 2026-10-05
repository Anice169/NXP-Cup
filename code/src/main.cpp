/*
 * ============================================================================
 *  Voiture autonome suiveuse de ligne (inspirée de la NXP Cup)
 *  Projet de 3e année - Polytech Dijon
 * ----------------------------------------------------------------------------
 *  Principe : au démarrage, on charge (ou on réalise) la calibration
 *  blanc/noir des capteurs. Ensuite, toutes les 50 ms, on repère le capteur
 *  qui voit le plus de noir et on oriente le servomoteur selon un angle
 *  prédéfini, pendant que les deux moteurs tournent à vitesse constante.
 *  La voiture s'arrête automatiquement au bout de DUREE_MAX.
 *
 *  Les valeurs que l'on peut modifier pour ajuster le comportement sont
 *  repérées par le marqueur [À RÉGLER]. Les voici :
 *    - VITESSE                    : vitesse des moteurs (0 à 255)
 *    - DUREE_MAX                  : durée avant l'arrêt automatique (ms)
 *    - le seuil de détection      : 300 sur 1000, dans loop()
 *    - les angles du servomoteur  : 70 / 90 / 130 / 150, dans loop()
 *    - la pause entre deux cycles : 50 ms, dans loop()
 *    - la fenêtre de recalibration (10 s) et le décompte de départ (3 s),
 *      dans setup()
 *    - le nombre de mesures moyennées : paramètre n de lireMoyenne()
 *    - le sens de rotation des moteurs, dans avancer()
 * ============================================================================
 */

#include <Arduino.h>
#include <ESP32Servo.h>    // Pilotage du servomoteur (version de la bibliothèque adaptée à l'ESP32)
#include <Preferences.h>   // Sauvegarde de valeurs en mémoire flash (zone NVS)

// ---------------------------------------------------------------------------
//  Brochage (numéros de GPIO de l'ESP32)
// ---------------------------------------------------------------------------

// Sorties analogiques du module de suivi de ligne.
// La voie 3 (centrale, GPIO 36) n'est volontairement pas utilisée : elle s'est
// révélée peu fiable. Les GPIO 34, 35 et 39 sont des entrées uniquement (ADC1).
// Le GPIO 4 est sur l'ADC2, utilisable ici car le Wi-Fi n'est pas activé.
#define CAPTEUR_1  34
#define CAPTEUR_2  35
#define CAPTEUR_4  39
#define CAPTEUR_5  4

// Signal de commande (PWM) du servomoteur de direction
#define PIN_SERVO  18

// Entrées du DRV8833 : A1 et A2 pour le moteur A (IN1 / IN2),
// B1 et B2 pour le moteur B (IN3 / IN4)
#define MOTEUR_A1  25
#define MOTEUR_A2  26
#define MOTEUR_B1  27
#define MOTEUR_B2  14

// ---------------------------------------------------------------------------
//  Variables et constantes globales
// ---------------------------------------------------------------------------

// Les 4 capteurs utilisés, dans l'ordre de gauche à droite (voies 1, 2, 4, 5)
const int capteurs[4] = {CAPTEUR_1, CAPTEUR_2, CAPTEUR_4, CAPTEUR_5};

// Valeurs brutes lues par chaque capteur sur le blanc et sur le noir
// (renseignées par la calibration, de 0 à 4095 car l'ADC de l'ESP32 est sur 12 bits)
int ref_blanc[4];
int ref_noir[4];

Servo monServo;      // Objet qui pilote le servomoteur
Preferences prefs;   // Objet qui lit/écrit dans la mémoire flash

// [À RÉGLER] Vitesse des moteurs : rapport cyclique PWM de 0 (arrêt) à 255 (maximum).
// 140 correspond à environ 55 %. Trop élevée, la voiture sort des virages.
const int VITESSE = 140;

// [À RÉGLER] Durée maximale de fonctionnement avant l'arrêt automatique de
// sécurité, en millisecondes (100000 ms = 100 secondes)
const int DUREE_MAX = 100000;

unsigned long tempsDepart;   // Instant (en ms) du départ de la voiture

// ---------------------------------------------------------------------------
//  Lecture des capteurs
// ---------------------------------------------------------------------------

// Lit un capteur n fois et renvoie la moyenne, pour réduire le bruit de mesure.
// [À RÉGLER] n = 50 mesures espacées de 2 ms (soit environ 0,1 s par capteur).
int lireMoyenne(int broche, int n = 50) {
  long total = 0;
  for (int i = 0; i < n; i++) {
    total += analogRead(broche);
    delay(2);
  }
  return total / n;
}

// ---------------------------------------------------------------------------
//  Calibration
// ---------------------------------------------------------------------------

// Mesure la valeur de chaque capteur sur une surface de référence ("BLANC" ou
// "NOIR") et la range dans le tableau valeurs[].
// Attention : la fonction est bloquante, elle attend un appui sur ENTRÉE dans
// le moniteur série. Elle n'est donc utilisable qu'avec l'USB branché.
void calibrer(int valeurs[4], const char *nom) {
  Serial.print("\n>>> Place le module sur ");
  Serial.print(nom);
  Serial.println(" puis appuie sur ENTREE...");
  while (Serial.available() == 0) { delay(10); }       // Attente de l'appui sur ENTRÉE
  while (Serial.available() > 0) { Serial.read(); }    // Vide la mémoire de réception
  Serial.println("Calibration en cours...");
  for (int i = 0; i < 4; i++) {
    valeurs[i] = lireMoyenne(capteurs[i]);
    Serial.print("Capteur ");
    Serial.print(i + 1);
    Serial.print(" : ");
    Serial.println(valeurs[i]);
  }
}

// Enregistre la calibration en mémoire flash pour qu'elle survive à une coupure
// d'alimentation. Les valeurs sont rangées dans l'espace de noms "calib" :
//   b0..b3 = valeurs sur le blanc, n0..n3 = valeurs sur le noir,
//   ok     = drapeau indiquant qu'une calibration existe.
// À n'appeler qu'après une calibration : le nombre d'écritures en flash est limité.
void sauvegarderCalibration() {
  prefs.begin("calib", false);   // false = lecture et écriture
  for (int i = 0; i < 4; i++) {
    prefs.putInt(("b" + String(i)).c_str(), ref_blanc[i]);
    prefs.putInt(("n" + String(i)).c_str(), ref_noir[i]);
  }
  prefs.putBool("ok", true);
  prefs.end();
  Serial.println("Calibration sauvegardee !");
}

// Recharge la calibration depuis la mémoire flash.
// Renvoie true si une calibration existait, false sinon (première utilisation).
bool chargerCalibration() {
  prefs.begin("calib", true);    // true = lecture seule
  bool ok = prefs.getBool("ok", false);
  if (ok) {
    for (int i = 0; i < 4; i++) {
      // Les deuxièmes arguments (0 et 1000) sont les valeurs par défaut
      // renvoyées si une clé est introuvable.
      ref_blanc[i] = prefs.getInt(("b" + String(i)).c_str(), 0);
      ref_noir[i]  = prefs.getInt(("n" + String(i)).c_str(), 1000);
    }
  }
  prefs.end();
  return ok;
}

// ---------------------------------------------------------------------------
//  Commande des moteurs
// ---------------------------------------------------------------------------
// Les 4 entrées du DRV8833 sont pilotées par des canaux PWM (LEDC) numérotés
// 4 à 7 : 4 = A1, 5 = A2, 6 = B1, 7 = B2 (voir setup()).
// Pour avancer, A1 et B1 reçoivent la vitesse tandis que A2 et B2 restent à 0.
// [À RÉGLER] Si un moteur tourne à l'envers, on peut inverser ses deux fils ou
// échanger les rôles des canaux 4/5 (moteur A) ou 6/7 (moteur B) ci-dessous.

void avancer(int vitesse) {
  ledcWrite(4, vitesse);
  ledcWrite(5, 0);
  ledcWrite(6, vitesse);
  ledcWrite(7, 0);
}

// Coupe les quatre entrées : les moteurs ne sont plus alimentés (roue libre)
void arreter() {
  ledcWrite(4, 0);
  ledcWrite(5, 0);
  ledcWrite(6, 0);
  ledcWrite(7, 0);
}

// ---------------------------------------------------------------------------
//  Initialisation (exécutée une seule fois à la mise sous tension)
// ---------------------------------------------------------------------------
void setup() {
  // Liaison série pour le débogage. La vitesse (115200) doit être la même que
  // "monitor_speed" dans platformio.ini.
  Serial.begin(115200);
  delay(500);

  // Configuration du PWM des moteurs avec l'ancienne API LEDC (Arduino core
  // ESP32 2.x) : ledcSetup(canal, fréquence en Hz, résolution en bits), puis
  // ledcAttachPin(broche, canal). Ici 1 kHz sur 8 bits (valeurs de 0 à 255).
  // Les canaux 4 à 7 évitent les premiers canaux, que la bibliothèque
  // ESP32Servo peut utiliser pour le servomoteur.
  ledcSetup(4, 1000, 8); ledcAttachPin(MOTEUR_A1, 4);
  ledcSetup(5, 1000, 8); ledcAttachPin(MOTEUR_A2, 5);
  ledcSetup(6, 1000, 8); ledcAttachPin(MOTEUR_B1, 6);
  ledcSetup(7, 1000, 8); ledcAttachPin(MOTEUR_B2, 7);
  arreter();   // Sécurité : moteurs à l'arrêt tant que la calibration n'est pas faite

  monServo.attach(PIN_SERVO);
  monServo.write(90);   // Direction centrée au démarrage

  if (chargerCalibration()) {
    // Une calibration est déjà enregistrée : on laisse la possibilité d'en refaire une
    Serial.println("Calibration chargee !");
    Serial.println("Envoie 'c' pour recalibrer, sinon depart dans 10s...");
    unsigned long t = millis();
    // [À RÉGLER] Fenêtre de recalibration : 10000 ms (10 secondes)
    while (millis() - t < 10000) {
      if (Serial.available() && Serial.read() == 'c') {
        Serial.println("=== Recalibration ===");
        calibrer(ref_blanc, "BLANC");
        calibrer(ref_noir, "NOIR");
        sauvegarderCalibration();
        break;
      }
    }
  } else {
    // Aucune calibration en mémoire (première utilisation) : on la réalise
    Serial.println("=== Premiere calibration ===");
    calibrer(ref_blanc, "BLANC");
    calibrer(ref_noir, "NOIR");
    sauvegarderCalibration();
  }

  // [À RÉGLER] Décompte avant le départ : 3000 ms (3 secondes), le temps de
  // poser la voiture sur la piste
  Serial.println("=== Depart dans 3 secondes ===");
  delay(3000);
  tempsDepart = millis();
}

// ---------------------------------------------------------------------------
//  Boucle principale (répétée en continu, environ 20 fois par seconde)
// ---------------------------------------------------------------------------
void loop() {
  // --- 1. Arrêt automatique de sécurité ------------------------------------
  // Une fois DUREE_MAX écoulée, on coupe les moteurs, on recentre la direction
  // et on bloque le programme (il faut redémarrer la carte pour repartir).
  if (millis() - tempsDepart > DUREE_MAX) {
    arreter();
    monServo.write(90);
    Serial.println("=== ARRET automatique ===");
    while (true);
  }

  // --- 2. Lecture et normalisation des capteurs ----------------------------
  // Chaque valeur brute est ramenée à une échelle de 0 (blanc) à 1000 (noir)
  // grâce aux valeurs de calibration. Cela compense les écarts entre capteurs.
  int norm[4];
  for (int i = 0; i < 4; i++) {
    int brut = analogRead(capteurs[i]);
    norm[i] = map(brut, ref_blanc[i], ref_noir[i], 0, 1000);
    norm[i] = constrain(norm[i], 0, 1000);   // Évite de sortir de l'intervalle 0-1000
  }

  // Affichage de débogage (utile pour régler les seuils avec l'USB branché)
  Serial.print("Capteurs: ");
  for (int i = 0; i < 4; i++) {
    Serial.print(norm[i]);
    Serial.print("\t");
  }

  // --- 3. Détection : recherche du capteur qui voit le plus de noir ---------
  int maxVal = 0;     // Plus grande valeur normalisée trouvée
  int maxIdx = -1;    // Indice du capteur correspondant (0 à 3)
  for (int i = 0; i < 4; i++) {
    if (norm[i] > maxVal) {
      maxVal = norm[i];
      maxIdx = i;
    }
  }

  // --- 4. Direction : choix de l'angle du servomoteur -----------------------
  // [À RÉGLER] Seuil de détection : sous 300 sur 1000, on considère qu'aucun
  // capteur ne voit la ligne et on va tout droit.
  //
  // [À RÉGLER] Angles du servomoteur (en degrés) : 70 / 90 / 130 / 150.
  // Ces valeurs ont été trouvées par essais. Elles ne sont pas symétriques
  // autour de 90° car le parallélisme des roues avant n'est pas parfait : avec
  // des angles symétriques, la voiture ne roulait pas droit. Si la voiture dérive
  // ou si la mécanique change, ce sont ces valeurs qu'il faut ajuster.
  // D'après le montage actuel, un angle inférieur à 90° braque à gauche et un
  // angle supérieur à 90° braque à droite. Les libellés affichés dans le
  // moniteur série ("Gauche", "Droite") sont indicatifs.
  int angle;
  if (maxVal < 300) {
    angle = 90;
    Serial.print("-> Tout droit");
  } else {
    switch (maxIdx) {
      case 0: angle = 70; Serial.print("-> Gauche max"); break;   // Capteur 1
      case 1: angle = 90; Serial.print("-> Gauche");     break;   // Capteur 2
      case 2: angle = 130;  Serial.print("-> Droite");     break; // Capteur 4
      case 3: angle = 150;  Serial.print("-> Droite max"); break; // Capteur 5
    }
  }

  // --- 5. Commande des actionneurs -----------------------------------------
  monServo.write(angle);   // Oriente la direction
  avancer(VITESSE);        // Les deux moteurs tournent à vitesse constante
  Serial.print(" -> Angle: ");
  Serial.println(angle);

  // [À RÉGLER] Pause entre deux cycles : 50 ms, soit environ 20 décisions par
  // seconde. Une pause plus courte rend la voiture plus réactive, mais envoie
  // plus de messages sur la liaison série.
  delay(50);
}
