#pragma once
#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

namespace Display {
  static LiquidCrystal_I2C lcd(0x3F, 16, 2);
  static bool inicial = true, ultimaVent = false;
  static float ultimaTemp = NAN;

  static void iniciar() {
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Iniciando...");
  }

  static void atualizar(float temperatura, bool vent) {
    bool mudouTemp = (isnan(temperatura) != isnan(ultimaTemp)) ||
                     (isfinite(temperatura) && temperatura != ultimaTemp);
    if (!inicial && !mudouTemp && vent == ultimaVent) return;
    inicial = false;
    ultimaTemp = temperatura;
    ultimaVent = vent;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    if (isnan(temperatura)) lcd.print("--.-");
    else lcd.print(temperatura, 1);
    lcd.print((char)223);
    lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print(vent ? "Ventilando..." : "Vent. Desligada");
  }
}
