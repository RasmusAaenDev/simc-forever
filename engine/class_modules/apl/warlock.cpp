#include "class_modules/apl/warlock.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace warlock_apl
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
//warlock_apl_start
void warlock( player_t* p )
{
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* default_  = p->get_action_priority_list( "default" );

  // AULA-RETTELSE: summon_demon tilkalder daemonen efter level og talenter (eller default_pet=). Med Demonic
  // Sacrifice og Demonic Pact tilkaldes og ofres Imp'en foerst, og den rigtige daemon tilkaldes bagefter.
  precombat->add_action( "summon_demon" );
  precombat->add_action( "demonic_sacrifice" );
  precombat->add_action( "summon_demon" );

  // AULA-RETTELSE: Life Tap ved lav mana (livet er uendeligt i simc; antagelse: en healer daekker det),
  // Bane of Agony og Siphon Life som DoTs, og spec-talenterne efter hvad warlocken har laert:
  // Destruction (Shadowburn, Conflagrate, Incinerate) og Affliction (Wrack). Wand naar intet andet kan kastes.
  default_->add_action( "life_tap,if=mana.pct<20" );
  default_->add_action( "shadowburn" );
  default_->add_action( "conflagrate,if=dot.immolate.ticking" );
  default_->add_action( "bane_of_agony,if=!ticking" );
  default_->add_action( "corruption,if=!ticking|remains<2" );
  default_->add_action( "siphon_life,if=!ticking" );
  default_->add_action( "immolate,if=!ticking|remains<2" );
  default_->add_action( "wrack,if=dot.corruption.ticking&dot.bane_of_agony.ticking" );
  default_->add_action( "incinerate,if=dot.immolate.ticking" );
  default_->add_action( "shadow_bolt" );
  default_->add_action( "shoot" );
}
//warlock_apl_end
}  // namespace warlock_apl
