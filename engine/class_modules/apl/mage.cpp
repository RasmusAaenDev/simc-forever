#include "class_modules/apl/mage.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace mage_apl
{
std::string potion( const player_t* )
{
  return "disabled";
}

std::string flask( const player_t* )
{
  return "disabled";
}

std::string food( const player_t* )
{
  return "disabled";
}

std::string rune( const player_t* )
{
  return "disabled";
}

std::string temporary_enchant( const player_t* )
{
  return "disabled";
}
//mage_apl_start
// AULA-RETTELSE: Rotationen vaelges ud fra de valgte talenter.
// - Frost (Ice Lance, Fingers of Frost eller Ice Barrier): Frost-basisrotationen nedenfor.
// - Fire (Ignite, Pyroblast, Improved Scorch, Heating Up eller Combustion, men ingen
//   Frost-noegletalenter): listen "fire".
// - Arcane (Arcane Blast, Arcane Wrath, Missile Barrage, Presence of Mind eller Arcane Power, men
//   ingen Frost- eller Fire-noegletalenter): listen "arcane".
// - Ingen af delene: grundrotationen (Fire Blast paa nedkoeling, ellers Frostbolt; Fireball kun
//   foer Frostbolt er laert). Evocation naar manaen er lav; er der ikke mana til en spell, skydes
//   der med wand (Shoot). Talent-spells uden talentet er aldrig klar.
void mage( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );
  action_priority_list_t* fire     = p->get_action_priority_list( "fire" );
  action_priority_list_t* arcane   = p->get_action_priority_list( "arcane" );

  const std::string frost_key = "(talent.ice_lance.enabled|talent.fingers_of_frost.enabled|talent.ice_barrier.enabled)";
  const std::string fire_key  = "(talent.ignite.enabled|talent.pyroblast.enabled|talent.improved_scorch.enabled|"
                                "talent.heating_up.enabled|talent.combustion.enabled)";
  const std::string arcane_key = "(talent.arcane_blast.enabled|talent.arcane_wrath.enabled|talent.missile_barrage.enabled|"
                                 "talent.presence_of_mind.enabled|talent.arcane_power.enabled)";

  default_->add_action( "evocation,if=mana.pct<10" );
  default_->add_action( "call_action_list,name=fire,if=" + fire_key + "&!" + frost_key );
  default_->add_action( "call_action_list,name=arcane,if=" + arcane_key + "&!" + frost_key + "&!" + fire_key );
  default_->add_action( "ice_barrier,if=!buff.ice_barrier.up" );
  default_->add_action( "ice_lance,if=buff.fingers_of_frost.up|debuff.frozen.up" );
  default_->add_action( "fire_blast" );
  default_->add_action( "frostbolt" );
  default_->add_action( "fireball" );
  default_->add_action( "shoot" );

  // Fire: Pyroblast som aabner, Scorch til 5 stak Fire Vulnerability (og igen foer udloeb),
  // Combustion paa nedkoeling, Pyroblast ved 3 Heating Up, Fire Blast paa nedkoeling, Fireball.
  fire->add_action( "pyroblast,if=time=0" );
  fire->add_action( "scorch,if=talent.improved_scorch.enabled&(debuff.fire_vulnerability.stack<5|debuff.fire_vulnerability.remains<5)" );
  fire->add_action( "combustion" );
  fire->add_action( "pyroblast,if=buff.heating_up.stack>=3" );
  fire->add_action( "fire_blast" );
  fire->add_action( "arcane_missiles,if=buff.missile_barrage.up" );
  fire->add_action( "fireball" );
  fire->add_action( "shoot" );

  // Arcane: parametre som variable (aendres med en egen actions-liste):
  //   arcane_blast_max_stacks = hoejst saa mange Arcane Blast-stakke af mana-hensyn (Mobalytics: 2);
  //   missiles_need_barrage   = 1: Arcane Missiles kun paa Missile Barrage (gratis og hurtig).
  // Clearcasting bruges paa Arcane Blast; Arcane Missiles paa Missile Barrage; Presence of Mind
  // paa Arcane Wrath; Arcane Power paa nedkoeling; Fire Blast paa nedkoeling; Arcane Blast op til
  // graensen; ellers Frostbolt (fisker efter Missile Barrage) fra grundrotationen nedenfor.
  arcane->add_action( "variable,name=arcane_blast_max_stacks,value=2" );
  arcane->add_action( "variable,name=missiles_need_barrage,value=1" );
  arcane->add_action( "arcane_power" );
  arcane->add_action( "arcane_blast,if=buff.clearcasting.up" );
  arcane->add_action( "arcane_missiles,if=buff.missile_barrage.up|!variable.missiles_need_barrage" );
  arcane->add_action( "presence_of_mind,if=!buff.clearcasting.up&(!talent.arcane_wrath.enabled|cooldown.arcane_wrath.ready&mana>=action.arcane_wrath.cost)" );
  arcane->add_action( "arcane_wrath" );
  arcane->add_action( "fire_blast" );
  arcane->add_action( "arcane_blast,if=buff.arcane_blast.stack<variable.arcane_blast_max_stacks" );
}
//mage_apl_end
}  // namespace mage_apl
