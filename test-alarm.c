#include <glib.h>
#include "alarm-notify.h"

static void
alarm_cb (AlarmNotify *notify, ECalComponent *comp)
{
  ECalComponentText text;

  e_cal_component_get_summary (comp, &text);
  g_print ("Got alarm for %s\n", text.value);
}

int
main (int argc, char **argv)
{
  GMainLoop *loop;
  AlarmNotify *notify;

  g_type_init ();

  loop = g_main_loop_new (NULL, TRUE);

  notify = alarm_notify_new ();
  g_signal_connect (notify, "alarm", G_CALLBACK (alarm_cb), NULL);

  g_main_loop_run (loop);

  return 0;
}
