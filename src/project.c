/*
 *      project.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2007 The Geany contributors
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file project.h
 * Project Management.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "project.h"

#include "app.h"
#include "build.h"
#include "dialogs.h"
#include "document.h"
#include "editor.h"
#include "filetypesprivate.h"
#include "geanyobject.h"
#include "keyfile.h"
#include "main.h"
#include "projectprivate.h"
#include "sidebar.h"
#include "stash.h"
#include "support.h"
#include "ui_utils.h"
#include "utils.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>


ProjectPrefs project_prefs = { NULL, FALSE };


static GeanyProjectPrivate priv;
static GeanyIndentPrefs indentation;

static GSList *stash_groups = NULL;

static struct
{
	gchar *project_file_path; /* in UTF-8 */
} local_prefs = { NULL };

/* simple struct to keep references to the elements of the properties dialog */
typedef struct _PropertyDialogElements
{
	GtkWidget *dialog;
	GtkWidget *notebook;
	GtkWidget *name;
	GtkWidget *description;
	GtkWidget *file_name;
	GtkWidget *base_path;
	GtkWidget *patterns;
	BuildTableData build_properties;
	gint build_page_num;
	gboolean entries_modified;
} PropertyDialogElements;


static gboolean update_config(const PropertyDialogElements *e, gboolean new_project);
static void on_file_save_button_clicked(GtkButton *button, PropertyDialogElements *e);
static gboolean load_config(const gchar *filename);
static gboolean write_config(void);
static void update_new_project_dlg(GtkEditable *editable, PropertyDialogElements *e,
	const gchar *base_p);
static void on_name_entry_changed(GtkEditable *editable, PropertyDialogElements *e);
static void on_entries_changed(GtkEditable *editable, PropertyDialogElements *e);
static void on_radio_long_line_custom_toggled(GtkToggleButton *radio, GtkWidget *spin_long_line);
static void run_new_dialog(PropertyDialogElements *e);
static void apply_editor_prefs(void);
static void init_stash_prefs(void);
static void destroy_project(gboolean open_default);


#define SHOW_ERR(args) dialogs_show_msgbox(GTK_MESSAGE_ERROR, args)
#define SHOW_ERR1(args, more) dialogs_show_msgbox(GTK_MESSAGE_ERROR, args, more)
#define MAX_NAME_LEN 50
/* Translators: "projects" is part of the default project base path so be careful when translating
 * please avoid special characters and spaces, look at the source for details or ask Frank */
#define PROJECT_DIR _("projects")


// returns whether we have working documents open
static gboolean have_session_docs(void)
{
	gint npages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook));
	GeanyDocument *doc = document_get_current();

	return npages > 1 || (npages == 1 && (doc->file_name || doc->changed));
}


static gboolean handle_current_session(void)
{
	if (!app->project)
	{
		/* save session in case the dialog is cancelled */
		configuration_save_default_session();
		/* don't ask if the only doc is an unmodified new doc */
		if (have_session_docs())
		{
			if (dialogs_show_question(
				_("Move the current documents into the new project's session?")))
			{
				// don't reload session on closing project
				configuration_clear_default_session();
			}
			else
			{
				if (!document_close_all())
					return FALSE;
			}
		}
	}
	if (app->project)
		return project_close(FALSE);
	return TRUE;
}


/* TODO: this should be ported to Glade like the project preferences dialog,
 * then we can get rid of the PropertyDialogElements struct altogether as
 * widgets pointers can be accessed through ui_lookup_widget(). */
void project_new(gboolean from_folder)
{
	GtkWidget *vbox;
	GtkWidget *table;
	GtkWidget *image;
	GtkWidget *button;
	GtkWidget *bbox;
	GtkWidget *label;
	gchar *tooltip;
	gchar *base_path = NULL;
	PropertyDialogElements e = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, FALSE };

	if (from_folder)
	{
		GeanyDocument *doc = document_get_current();
		gchar *start_path;

		if (doc && doc->file_name)
			start_path = g_path_get_dirname(doc->file_name);
		else if (!EMPTY(local_prefs.project_file_path))
			start_path = g_strdup(local_prefs.project_file_path);
		else
			start_path = utils_get_utf8_from_locale(g_get_home_dir());

		base_path = ui_get_project_directory(start_path);
		g_free(start_path);

		if (!base_path)
			return;
	}

	e.dialog = gtk_dialog_new_with_buttons(_("New Project"), GTK_WINDOW(main_widgets.window),
										 GTK_DIALOG_DESTROY_WITH_PARENT,
										 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);

	gtk_widget_set_name(e.dialog, "GeanyDialogProject");
	button = ui_button_new_with_image(GTK_STOCK_NEW, _("C_reate"));
	gtk_widget_set_can_default(button, TRUE);
	gtk_window_set_default(GTK_WINDOW(e.dialog), button);
	gtk_dialog_add_action_widget(GTK_DIALOG(e.dialog), button, GTK_RESPONSE_OK);

	vbox = ui_dialog_vbox_new(GTK_DIALOG(e.dialog));

	table = gtk_table_new(3, 2, FALSE);
	gtk_table_set_row_spacings(GTK_TABLE(table), 5);
	gtk_table_set_col_spacings(GTK_TABLE(table), 10);

	label = gtk_label_new(_("Name:"));
	gtk_misc_set_alignment(GTK_MISC(label), 1, 0);

	e.name = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(e.name), TRUE);
	ui_entry_add_clear_icon(GTK_ENTRY(e.name));
	gtk_entry_set_max_length(GTK_ENTRY(e.name), MAX_NAME_LEN);
	gtk_widget_set_tooltip_text(e.name, _("Project name"));

	ui_table_add_row(GTK_TABLE(table), 0, label, e.name, NULL);

	label = gtk_label_new(_("Filename:"));
	gtk_misc_set_alignment(GTK_MISC(label), 1, 0);

	e.file_name = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(e.file_name), TRUE);
	ui_entry_add_clear_icon(GTK_ENTRY(e.file_name));
	gtk_entry_set_width_chars(GTK_ENTRY(e.file_name), 40);
	tooltip = g_strdup_printf(
		_("Path of the file representing the project and storing its settings. "
		"It should normally have the \"%s\" extension."), "."GEANY_PROJECT_EXT);
	gtk_widget_set_tooltip_text(e.file_name, tooltip);
	g_free(tooltip);
	button = gtk_button_new();
	g_signal_connect(button, "clicked", G_CALLBACK(on_file_save_button_clicked), &e);
	image = gtk_image_new_from_stock(GTK_STOCK_OPEN, GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(button), image);
	bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(bbox), e.file_name, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(bbox), button, FALSE, FALSE, 0);

	ui_table_add_row(GTK_TABLE(table), 1, label, bbox, NULL);

	label = gtk_label_new(_("Base path:"));
	gtk_misc_set_alignment(GTK_MISC(label), 1, 0);

	e.base_path = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(e.base_path), TRUE);
	ui_entry_add_clear_icon(GTK_ENTRY(e.base_path));
	gtk_widget_set_tooltip_text(e.base_path,
		_("Base directory of all files that make up the project. "
		"This can be a new path, or an existing directory tree. "
		"You can use paths relative to the project filename."));
	bbox = ui_path_box_new(_("Choose Project Base Path"),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_ENTRY(e.base_path));

	ui_table_add_row(GTK_TABLE(table), 2, label, bbox, NULL);

	gtk_box_pack_start(GTK_BOX(vbox), table, TRUE, TRUE, 0);

	if (base_path)
	{
		update_new_project_dlg(GTK_EDITABLE(e.name), &e, base_path);
		g_free(base_path);
	}
	else
	{
		/* signals */
		g_signal_connect(e.name, "changed", G_CALLBACK(on_name_entry_changed), &e);
		g_signal_connect(e.file_name, "changed", G_CALLBACK(on_entries_changed), &e);
		g_signal_connect(e.base_path, "changed", G_CALLBACK(on_entries_changed), &e);

		update_new_project_dlg(GTK_EDITABLE(e.name), &e, NULL);
	}

	gtk_widget_show_all(e.dialog);
	run_new_dialog(&e);
	gtk_widget_destroy(e.dialog);
	document_new_file_if_non_open();
	ui_focus_current_document();
}


static void run_new_dialog(PropertyDialogElements *e)
{
	if (gtk_dialog_run(GTK_DIALOG(e->dialog)) != GTK_RESPONSE_OK ||
		!handle_current_session())
		return;
	do
	{
		if (update_config(e, TRUE))
		{
			// app->project is now set
			if (!write_config())
			{
				SHOW_ERR(_("Project file could not be written"));
				destroy_project(FALSE);
			}
			else
			{
				ui_set_statusbar(TRUE, _("Project \"%s\" created."), app->project->name);
				ui_add_recent_project_file(app->project->file_name);
				return;
			}
		}
	}
	while (gtk_dialog_run(GTK_DIALOG(e->dialog)) == GTK_RESPONSE_OK);
	// any open docs were meant to be moved into the project
	// rewrite default session because it was cleared
	if (have_session_docs())
		configuration_save_default_session();
	else
	{
		// reload any documents that were closed
		configuration_load_default_session();
		configuration_open_default_session();
	}
}


gboolean project_load_file_with_session(const gchar *locale_file_name)
{
	if (project_load_file(locale_file_name))
	{
		configuration_open_files(app->project->priv->session_files);
		app->project->priv->session_files = NULL;
		document_new_file_if_non_open();
		ui_focus_current_document();
		return TRUE;
	}
	return FALSE;
}


static void run_open_dialog(GtkFileChooser *dialog)
{
	while (dialogs_file_chooser_run(dialog) == GTK_RESPONSE_ACCEPT)
	{
		gchar *filename = gtk_file_chooser_get_filename(dialog);

		if (app->project && !project_close(FALSE)) {}
		/* try to load the config */
		else if (! project_load_file_with_session(filename))
		{
			gchar *utf8_filename = utils_get_utf8_from_locale(filename);

			SHOW_ERR1(_("Project file \"%s\" could not be loaded."), utf8_filename);
			if (GTK_IS_WIDGET(dialog))
				gtk_widget_grab_focus(GTK_WIDGET(dialog));
			g_free(utf8_filename);
			g_free(filename);
			continue;
		}
		g_free(filename);
		break;
	}
}


void project_open(void)
{
	const gchar *dir = local_prefs.project_file_path;
	gchar *locale_path;
	GtkFileChooser *dialog;
	GtkFileFilter *filter;

	if (interface_prefs.use_native_windows_dialogs)
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_native_new(_("Open Project"),
			GTK_WINDOW(main_widgets.window), GTK_FILE_CHOOSER_ACTION_OPEN, NULL, NULL));
	else
	{
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new(_("Open Project"), GTK_WINDOW(main_widgets.window),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL));
		gtk_widget_set_name(GTK_WIDGET(dialog), "GeanyDialogProject");

		/* set default Open, so pressing enter can open multiple files */
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
		gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
		gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main_widgets.window));
		gtk_widget_show_all(GTK_WIDGET(dialog));
	}

	gtk_file_chooser_set_select_multiple(dialog, TRUE);

	/* add FileFilters */
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, _("All files"));
	gtk_file_filter_add_pattern(filter, "*");
	gtk_file_chooser_add_filter(dialog, filter);
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, _("Project files"));
	gtk_file_filter_add_pattern(filter, "*." GEANY_PROJECT_EXT);
	gtk_file_chooser_add_filter(dialog, filter);
	gtk_file_chooser_set_filter(dialog, filter);

	locale_path = utils_get_locale_from_utf8(dir);
	if (g_file_test(locale_path, G_FILE_TEST_EXISTS) &&
		g_file_test(locale_path, G_FILE_TEST_IS_DIR))
	{
		gtk_file_chooser_set_current_folder(dialog, locale_path);
	}
	g_free(locale_path);

	run_open_dialog(dialog);
	dialogs_file_chooser_destroy(dialog);
}


/* Called when creating, opening, closing and updating projects. */
static void update_ui(void)
{
	if (main_status.quitting)
		return;

	ui_set_window_title(NULL);
	build_menu_update(NULL);
	// update project name
	sidebar_openfiles_update_all();
	ui_update_recent_project_menu();
}


static void remove_foreach_project_filetype(gpointer data, gpointer user_data)
{
	GeanyFiletype *ft = data;
	if (ft != NULL)
	{
		SETPTR(ft->priv->projfilecmds, NULL);
		SETPTR(ft->priv->projexeccmds, NULL);
		SETPTR(ft->priv->projerror_regex_string, NULL);
		ft->priv->project_list_entry = -1;
	}
}


/* open_default will make function reload default session files on close */
gboolean project_close(gboolean open_default)
{
	g_return_val_if_fail(app->project != NULL, FALSE);

	/* save project session files, etc */
	if (!write_config())
		g_warning("Project file \"%s\" could not be written", app->project->file_name);

	/* close all existing tabs first */
	if (!document_close_all())
		return FALSE;

	ui_set_statusbar(TRUE, _("Project \"%s\" closed."), app->project->name);
	destroy_project(open_default);
	return TRUE;
}


static void destroy_project(gboolean open_default)
{
	GSList *node;

	g_return_if_fail(app->project != NULL);

	g_signal_emit_by_name(geany_object, "project-before-close");

	/* remove project filetypes build entries */
	if (app->project->priv->build_filetypes_list != NULL)
	{
		g_ptr_array_foreach(app->project->priv->build_filetypes_list, remove_foreach_project_filetype, NULL);
		g_ptr_array_free(app->project->priv->build_filetypes_list, FALSE);
	}

	/* remove project non filetype build menu items */
	build_remove_menu_item(GEANY_BCS_PROJ, GEANY_GBG_NON_FT, -1);
	build_remove_menu_item(GEANY_BCS_PROJ, GEANY_GBG_EXEC, -1);

	g_free(app->project->name);
	g_free(app->project->description);
	g_free(app->project->file_name);
	g_free(app->project->base_path);
	g_strfreev(app->project->file_patterns);

	g_free(app->project);
	app->project = NULL;

	foreach_slist(node, stash_groups)
		stash_group_free(node->data);

	g_slist_free(stash_groups);
	stash_groups = NULL;

	apply_editor_prefs(); /* ensure that global settings are restored */

	/* after closing all tabs let's open the tabs found in the default config */
	if (open_default && cl_options.load_session)
	{
		configuration_load_default_session();
		configuration_open_default_session();
		document_new_file_if_non_open();
		ui_focus_current_document();
	}
	g_signal_emit_by_name(geany_object, "project-close");

	update_ui();
}


/* Shows the file chooser dialog when base path button is clicked
 * FIXME: this should be connected in Glade but 3.8.1 has a bug
 * where it won't pass any objects as user data (#588824). */
static void on_project_properties_base_path_button_clicked(GtkWidget *button,
	GtkWidget *base_path_entry)
{
	GtkFileChooser *dialog;

	g_return_if_fail(base_path_entry != NULL);
	g_return_if_fail(GTK_IS_WIDGET(base_path_entry));

	if (interface_prefs.use_native_windows_dialogs)
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_native_new(_("Choose Project Base Path"),
			NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, NULL, NULL));
	else
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new(_("Choose Project Base Path"),
			NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
			NULL));

	if (dialogs_file_chooser_run(dialog) == GTK_RESPONSE_ACCEPT)
	{
		gtk_entry_set_text(GTK_ENTRY(base_path_entry),
			gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
	}

	dialogs_file_chooser_destroy(dialog);
}


static void insert_build_page(PropertyDialogElements *e)
{
	GtkWidget *build_table, *label;
	GeanyDocument *doc = document_get_current();
	GeanyFiletype *ft = NULL;

	if (doc != NULL)
		ft = doc->file_type;

	build_table = build_commands_table(doc, GEANY_BCS_PROJ, &(e->build_properties), ft);
	gtk_container_set_border_width(GTK_CONTAINER(build_table), 6);
	label = gtk_label_new(_("Build"));
	e->build_page_num = gtk_notebook_append_page(GTK_NOTEBOOK(e->notebook),
		build_table, label);
}


static void create_properties_dialog(PropertyDialogElements *e)
{
	GtkWidget *wid;
	static guint base_path_button_handler_id = 0;
	static guint radio_long_line_handler_id = 0;

	e->dialog = create_project_dialog();
	e->notebook = ui_lookup_widget(e->dialog, "project_notebook");
	e->file_name = ui_lookup_widget(e->dialog, "label_project_dialog_filename");
	e->name = ui_lookup_widget(e->dialog, "entry_project_dialog_name");
	e->description = ui_lookup_widget(e->dialog, "textview_project_dialog_description");
	e->base_path = ui_lookup_widget(e->dialog, "entry_project_dialog_base_path");
	e->patterns = ui_lookup_widget(e->dialog, "entry_project_dialog_file_patterns");

	gtk_entry_set_max_length(GTK_ENTRY(e->name), MAX_NAME_LEN);

	ui_entry_add_clear_icon(GTK_ENTRY(e->name));
	ui_entry_add_clear_icon(GTK_ENTRY(e->base_path));
	ui_entry_add_clear_icon(GTK_ENTRY(e->patterns));

	/* Workaround for bug in Glade 3.8.1, see comment above signal handler */
	if (base_path_button_handler_id == 0)
	{
		wid = ui_lookup_widget(e->dialog, "button_project_dialog_base_path");
		base_path_button_handler_id =
			g_signal_connect(wid, "clicked",
				G_CALLBACK(on_project_properties_base_path_button_clicked),
				e->base_path);
	}

	/* Same as above, should be in Glade but can't due to bug in 3.8.1 */
	if (radio_long_line_handler_id == 0)
	{
		wid = ui_lookup_widget(e->dialog, "radio_long_line_custom_project");
		radio_long_line_handler_id =
			g_signal_connect(wid, "toggled",
				G_CALLBACK(on_radio_long_line_custom_toggled),
				ui_lookup_widget(e->dialog, "spin_long_line_project"));
	}
}


static void show_project_properties(gboolean show_build)
{
	GeanyProject *p = app->project;
	GtkWidget *widget = NULL;
	GtkWidget *radio_long_line_custom;
	static PropertyDialogElements e;
	GSList *node;
	gchar *entry_text;
	GtkTextBuffer *buffer;

	g_return_if_fail(app->project != NULL);

	if (e.dialog == NULL)
		create_properties_dialog(&e);

	insert_build_page(&e);

	foreach_slist(node, stash_groups)
		stash_group_display(node->data, e.dialog);

	/* fill the elements with the appropriate data */
	gtk_entry_set_text(GTK_ENTRY(e.name), p->name);
	gtk_label_set_text(GTK_LABEL(e.file_name), p->file_name);
	gtk_entry_set_text(GTK_ENTRY(e.base_path), p->base_path);

	radio_long_line_custom = ui_lookup_widget(e.dialog, "radio_long_line_custom_project");
	switch (p->priv->long_line_behaviour)
	{
		case 0: widget = ui_lookup_widget(e.dialog, "radio_long_line_disabled_project"); break;
		case 1: widget = ui_lookup_widget(e.dialog, "radio_long_line_default_project"); break;
		case 2: widget = radio_long_line_custom; break;
	}
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);

	widget = ui_lookup_widget(e.dialog, "spin_long_line_project");
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), (gdouble)p->priv->long_line_column);
	on_radio_long_line_custom_toggled(GTK_TOGGLE_BUTTON(radio_long_line_custom), widget);

	/* set text */
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e.description));
	gtk_text_buffer_set_text(buffer, p->description ? p->description : "", -1);

	/* set the file patterns */
	entry_text = p->file_patterns ? g_strjoinv(" ", p->file_patterns) : g_strdup("");
	gtk_entry_set_text(GTK_ENTRY(e.patterns), entry_text);
	g_free(entry_text);

	g_signal_emit_by_name(geany_object, "project-dialog-open", e.notebook);
	gtk_widget_show_all(e.dialog);

	/* note: notebook page must be shown before setting current page */
	if (show_build)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(e.notebook), e.build_page_num);
	else
		gtk_notebook_set_current_page(GTK_NOTEBOOK(e.notebook), 0);

	while (gtk_dialog_run(GTK_DIALOG(e.dialog)) == GTK_RESPONSE_OK)
	{
		if (update_config(&e, FALSE))
		{
			g_signal_emit_by_name(geany_object, "project-dialog-confirmed", e.notebook);
			if (!write_config())
				SHOW_ERR(_("Project file could not be written"));
			else
			{
				ui_set_statusbar(TRUE, _("Project \"%s\" saved."), app->project->name);
				break;
			}
		}
	}

	build_free_fields(e.build_properties);
	g_signal_emit_by_name(geany_object, "project-dialog-close", e.notebook);
	gtk_notebook_remove_page(GTK_NOTEBOOK(e.notebook), e.build_page_num);
	gtk_widget_hide(e.dialog);
}


void project_properties(void)
{
	show_project_properties(FALSE);
}


void project_build_properties(void)
{
	show_project_properties(TRUE);
}


/* checks whether there is an already open project and asks the user if he wants to close it or
 * abort the current action. Returns FALSE when the current action(the caller) should be cancelled
 * and TRUE if we can go ahead */
gboolean project_ask_close(void)
{
	if (app->project != NULL)
	{
		if (!interface_prefs.warn_on_project_close ||
			dialogs_show_question_full(NULL, GTK_STOCK_CLOSE, GTK_STOCK_CANCEL,
			_("Do you want to close it before proceeding?"),
			_("The '%s' project is open."), app->project->name))
		{
			return project_close(FALSE);
		}
		else
			return FALSE;
	}
	else
		return TRUE;
}


static GeanyProject *create_project(void)
{
	GeanyProject *project = g_new0(GeanyProject, 1);

	memset(&priv, 0, sizeof priv);
	priv.indentation = &indentation;
	project->priv = &priv;

	init_stash_prefs();

	project->file_patterns = NULL;

	project->priv->long_line_behaviour = 1 /* use global settings */;
	project->priv->long_line_column = editor_prefs.long_line_column;

	app->project = project;
	return project;
}


/* Verifies data for New & Properties dialogs.
 * Creates app->project if NULL.
 * Returns: FALSE if the user needs to change any data. */
static gboolean update_config(const PropertyDialogElements *e, gboolean new_project)
{
	const gchar *name, *file_name, *base_path;
	gchar *locale_filename;
	gsize name_len;
	gint err_code = 0;
	GeanyProject *p;

	g_return_val_if_fail(e != NULL, TRUE);

	name = gtk_entry_get_text(GTK_ENTRY(e->name));
	name_len = strlen(name);
	if (name_len == 0)
	{
		SHOW_ERR(_("The specified project name is too short."));
		gtk_widget_grab_focus(e->name);
		return FALSE;
	}
	else if (name_len > MAX_NAME_LEN)
	{
		SHOW_ERR1(_("The specified project name is too long (max. %d characters)."), MAX_NAME_LEN);
		gtk_widget_grab_focus(e->name);
		return FALSE;
	}

	if (new_project)
		file_name = gtk_entry_get_text(GTK_ENTRY(e->file_name));
	else
		file_name = gtk_label_get_text(GTK_LABEL(e->file_name));

	if (G_UNLIKELY(EMPTY(file_name)))
	{
		SHOW_ERR(_("You have specified an invalid project filename."));
		gtk_widget_grab_focus(e->file_name);
		return FALSE;
	}

	locale_filename = utils_get_locale_from_utf8(file_name);
	base_path = gtk_entry_get_text(GTK_ENTRY(e->base_path));
	if (!EMPTY(base_path))
	{	/* check whether the given directory actually exists */
		gchar *locale_path = utils_get_locale_from_utf8(base_path);

		if (! g_path_is_absolute(locale_path))
		{	/* relative base path, so add base dir of project file name */
			gchar *dir = g_path_get_dirname(locale_filename);
			SETPTR(locale_path, g_build_filename(dir, locale_path, NULL));
			g_free(dir);
		}

		if (! g_file_test(locale_path, G_FILE_TEST_IS_DIR))
		{
			gboolean create_dir;

			create_dir = dialogs_show_question_full(NULL, GTK_STOCK_OK, GTK_STOCK_CANCEL,
				_("Create the project's base path directory?"),
				_("The path \"%s\" does not exist."),
				base_path);

			if (create_dir)
				err_code = utils_mkdir(locale_path, TRUE);

			if (! create_dir || err_code != 0)
			{
				if (err_code != 0)
					SHOW_ERR1(_("Project base directory could not be created (%s)."),
						g_strerror(err_code));
				gtk_widget_grab_focus(e->base_path);
				utils_free_pointers(2, locale_path, locale_filename, NULL);
				return FALSE;
			}
		}
		g_free(locale_path);
	}
	/* finally test whether the given project file can be written */
	if ((err_code = utils_is_file_writable(locale_filename)) != 0 ||
		(err_code = g_file_test(locale_filename, G_FILE_TEST_IS_DIR) ? EISDIR : 0) != 0)
	{
		SHOW_ERR1(_("Project file could not be written (%s)."), g_strerror(err_code));
		gtk_widget_grab_focus(e->file_name);
		g_free(locale_filename);
		return FALSE;
	}
	else if (new_project && g_file_test(locale_filename, G_FILE_TEST_EXISTS) &&
			 ! dialogs_show_question_full(NULL, _("_Replace"), GTK_STOCK_CANCEL,
				NULL,
				_("The file '%s' already exists. Do you want to overwrite it?"),
				file_name))
	{
		gtk_widget_grab_focus(e->file_name);
		g_free(locale_filename);
		return FALSE;
	}
	g_free(locale_filename);

	if (app->project == NULL)
	{
		create_project();
		new_project = TRUE;
	}
	p = app->project;

	SETPTR(p->name, g_strdup(name));
	SETPTR(p->file_name, g_strdup(file_name));
	/* use "." if base_path is empty */
	SETPTR(p->base_path, g_strdup(!EMPTY(base_path) ? base_path : "./"));

	if (! new_project)	/* save properties specific fields */
	{
		GtkTextIter start, end;
		GtkTextBuffer *buffer;
		GeanyDocument *doc = document_get_current();
		GeanyBuildCommand *oldvalue;
		GeanyFiletype *ft = doc ? doc->file_type : NULL;
		GtkWidget *widget;
		gchar *tmp;
		GString *str;
		GSList *node;

		/* get and set the project description */
		buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->description));
		gtk_text_buffer_get_start_iter(buffer, &start);
		gtk_text_buffer_get_end_iter(buffer, &end);
		SETPTR(p->description, gtk_text_buffer_get_text(buffer, &start, &end, FALSE));

		foreach_slist(node, stash_groups)
			stash_group_update(node->data, e->dialog);

		/* read the project build menu */
		oldvalue = ft ? ft->priv->projfilecmds : NULL;
		build_read_project(ft, e->build_properties);

		if (ft != NULL && ft->priv->projfilecmds != oldvalue && ft->priv->project_list_entry < 0)
		{
			if (p->priv->build_filetypes_list == NULL)
				p->priv->build_filetypes_list = g_ptr_array_new();
			ft->priv->project_list_entry = p->priv->build_filetypes_list->len;
			g_ptr_array_add(p->priv->build_filetypes_list, ft);
		}
		build_menu_update(doc);

		widget = ui_lookup_widget(e->dialog, "radio_long_line_disabled_project");
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
			p->priv->long_line_behaviour = 0;
		else
		{
			widget = ui_lookup_widget(e->dialog, "radio_long_line_default_project");
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
				p->priv->long_line_behaviour = 1;
			else
				/* "Custom" radio button must be checked */
				p->priv->long_line_behaviour = 2;
		}

		widget = ui_lookup_widget(e->dialog, "spin_long_line_project");
		p->priv->long_line_column = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
		apply_editor_prefs();

		/* get and set the project file patterns */
		tmp = g_strdup(gtk_entry_get_text(GTK_ENTRY(e->patterns)));
		g_strfreev(p->file_patterns);
		g_strstrip(tmp);
		str = g_string_new(tmp);
		do {} while (utils_string_replace_all(str, "  ", " "));
		p->file_patterns = g_strsplit(str->str, " ", -1);
		g_string_free(str, TRUE);
		g_free(tmp);
	}

	update_ui();

	return TRUE;
}


static void run_dialog(GtkFileChooser *dialog, GtkWidget *entry)
{
	/* set filename in the file chooser dialog */
	const gchar *utf8_filename = gtk_entry_get_text(GTK_ENTRY(entry));
	gchar *locale_filename = utils_get_locale_from_utf8(utf8_filename);

	if (g_path_is_absolute(locale_filename))
	{
		if (g_file_test(locale_filename, G_FILE_TEST_EXISTS))
		{
			/* if the current filename is a directory, we must use
			 * gtk_file_chooser_set_current_folder(which expects a locale filename) otherwise
			 * we end up in the parent directory */
			if (g_file_test(locale_filename, G_FILE_TEST_IS_DIR))
				gtk_file_chooser_set_current_folder(dialog, locale_filename);
			else
				gtk_file_chooser_set_filename(dialog, utf8_filename);
		}
		else /* if the file doesn't yet exist, use at least the current directory */
		{
			gchar *locale_dir = g_path_get_dirname(locale_filename);
			gchar *name = g_path_get_basename(utf8_filename);

			if (g_file_test(locale_dir, G_FILE_TEST_EXISTS))
				gtk_file_chooser_set_current_folder(dialog, locale_dir);
			gtk_file_chooser_set_current_name(dialog, name);

			g_free(name);
			g_free(locale_dir);
		}
	}
	else if (gtk_file_chooser_get_action(dialog) != GTK_FILE_CHOOSER_ACTION_OPEN)
	{
		gtk_file_chooser_set_current_name(dialog, utf8_filename);
	}
	g_free(locale_filename);

	/* run it */
	if (dialogs_file_chooser_run(dialog) == GTK_RESPONSE_ACCEPT)
	{
		gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		gchar *tmp_utf8_filename = utils_get_utf8_from_locale(filename);

		gtk_entry_set_text(GTK_ENTRY(entry), tmp_utf8_filename);

		g_free(tmp_utf8_filename);
		g_free(filename);
	}
	dialogs_file_chooser_destroy(dialog);
}


static void on_file_save_button_clicked(GtkButton *button, PropertyDialogElements *e)
{
	GtkFileChooser *dialog;

	/* initialise the dialog */
	if (interface_prefs.use_native_windows_dialogs)
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_native_new(_("Choose Project Filename"),
			NULL, GTK_FILE_CHOOSER_ACTION_SAVE, NULL, NULL));
	else
	{
		dialog = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new(_("Choose Project Filename"), NULL,
						GTK_FILE_CHOOSER_ACTION_SAVE,
						GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL));
		gtk_widget_set_name(GTK_WIDGET(dialog), "GeanyDialogProject");
		gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	}

	run_dialog(dialog, e->file_name);
}


/* sets the New Project dialog entries according to the base path or project name */
static void update_new_project_dlg(GtkEditable *editable, PropertyDialogElements *e,
	const gchar *base_p)
{
	gchar *base_path;
	gchar *file_name;
	gchar *project_dir = NULL;

	if (e->entries_modified)
		return;

	if (!EMPTY(local_prefs.project_file_path))
		project_dir = g_strdup(local_prefs.project_file_path);
	else
	{
		GeanyDocument *doc = document_get_current();

		if (doc && doc->file_name)
			project_dir = g_path_get_dirname(doc->file_name);
		else
			project_dir = utils_get_utf8_from_locale(g_get_home_dir());
	}

	if (!EMPTY(base_p))
	{
		gchar *name = g_path_get_basename(base_p);

		base_path = g_strdup(base_p);
		gtk_entry_set_text(GTK_ENTRY(e->name), name);
		if (project_prefs.project_file_in_basedir)
			file_name = g_strconcat(base_path, G_DIR_SEPARATOR_S,
				name, "." GEANY_PROJECT_EXT, NULL);
		else
			file_name = g_strconcat(project_dir, G_DIR_SEPARATOR_S,
				name, "." GEANY_PROJECT_EXT, NULL);
		g_free(name);
	}
	else
	{
		gchar *name = gtk_editable_get_chars(editable, 0, -1);
		if (!EMPTY(name))
		{
			base_path = g_strconcat(project_dir, G_DIR_SEPARATOR_S,
				name, G_DIR_SEPARATOR_S, NULL);
			if (project_prefs.project_file_in_basedir)
				file_name = g_strconcat(project_dir, G_DIR_SEPARATOR_S, name, G_DIR_SEPARATOR_S,
					name, "." GEANY_PROJECT_EXT, NULL);
			else
				file_name = g_strconcat(project_dir, G_DIR_SEPARATOR_S,
					name, "." GEANY_PROJECT_EXT, NULL);
		}
		else
		{
			base_path = g_strconcat(project_dir, G_DIR_SEPARATOR_S, NULL);
			file_name = g_strconcat(project_dir, G_DIR_SEPARATOR_S, NULL);
		}
		g_free(name);
	}

	gtk_entry_set_text(GTK_ENTRY(e->base_path), base_path);
	gtk_entry_set_text(GTK_ENTRY(e->file_name), file_name);

	e->entries_modified = FALSE;

	g_free(base_path);
	g_free(file_name);
	g_free(project_dir);
}


static void on_name_entry_changed(GtkEditable *editable, PropertyDialogElements *e)
{
	update_new_project_dlg(editable, e, NULL);
}


static void on_entries_changed(GtkEditable *editable, PropertyDialogElements *e)
{
	e->entries_modified = TRUE;
}


static void on_radio_long_line_custom_toggled(GtkToggleButton *radio, GtkWidget *spin_long_line)
{
	gtk_widget_set_sensitive(spin_long_line, gtk_toggle_button_get_active(radio));
}


gboolean project_load_file(const gchar *locale_file_name)
{
	g_return_val_if_fail(locale_file_name != NULL, FALSE);

	if (load_config(locale_file_name))
	{
		gchar *utf8_filename = utils_get_utf8_from_locale(locale_file_name);

		ui_set_statusbar(TRUE, _("Project \"%s\" opened."), app->project->name);

		ui_add_recent_project_file(utf8_filename);
		g_free(utf8_filename);
		return TRUE;
	}
	else
	{
		gchar *utf8_filename = utils_get_utf8_from_locale(locale_file_name);

		ui_set_statusbar(TRUE, _("Project file \"%s\" could not be loaded."), utf8_filename);
		g_free(utf8_filename);
	}
	return FALSE;
}


/* Reads the given filename and creates a new project with the data found in the file.
 * At this point there should not be an already opened project in Geany otherwise it will just
 * return.
 * The filename is expected in the locale encoding. */
static gboolean load_config(const gchar *filename)
{
	GKeyFile *config;
	GeanyProject *p;
	GSList *node;

	/* there should not be an open project */
	g_return_val_if_fail(app->project == NULL && filename != NULL, FALSE);

	config = g_key_file_new();
	if (! g_key_file_load_from_file(config, filename, G_KEY_FILE_NONE, NULL))
	{
		g_key_file_free(config);
		return FALSE;
	}

	p = create_project();

	foreach_slist(node, stash_groups)
		stash_group_load_from_key_file(node->data, config);

	p->name = utils_get_setting_string(config, "project", "name", GEANY_STRING_UNTITLED);
	p->description = utils_get_setting_string(config, "project", "description", "");
	p->file_name = utils_get_utf8_from_locale(filename);
	p->base_path = utils_get_setting_string(config, "project", "base_path", "");
	p->file_patterns = g_key_file_get_string_list(config, "project", "file_patterns", NULL, NULL);

	p->priv->long_line_behaviour = utils_get_setting_integer(config, "long line marker",
		"long_line_behaviour", 1 /* follow global */);
	p->priv->long_line_column = utils_get_setting_integer(config, "long line marker",
		"long_line_column", editor_prefs.long_line_column);
	apply_editor_prefs();

	build_load_menu(config, GEANY_BCS_PROJ, (gpointer)p);
	/* save current (non-project) session (it could have been changed since program startup) */
	if (!main_status.opening_session_files)
	{
		/* Opening another project while some project is already opene causes
		 * that upon closing the first project, empty session is saved here.
		 * The check below prevents that but has a side-effect that when
		 * save_config_on_file_change=FALSE, the session with all closed files
		 * isn't saved when opening a project. */
		if (have_session_docs())
			configuration_save_default_session();
		/* now close all open files */
		document_close_all();
	}
	/* read session files so they can be opened with configuration_open_files() */
	p->priv->session_files = configuration_load_session_files(config);
	g_signal_emit_by_name(geany_object, "project-open", config);
	g_key_file_free(config);

	update_ui();
	return TRUE;
}


static void apply_editor_prefs(void)
{
	guint i;

	foreach_document(i)
		editor_apply_update_prefs(documents[i]->editor);
}


/* Write the project settings as well as the project session files into its configuration files.
 * Returns: TRUE if project file was written successfully. */
static gboolean write_config(void)
{
	GeanyProject *p;
	GKeyFile *config;
	gchar *filename;
	gchar *data;
	gboolean ret = FALSE;
	GSList *node;

	g_return_val_if_fail(app->project != NULL, FALSE);

	p = app->project;

	config = g_key_file_new();
	/* try to load an existing config to keep manually added comments */
	filename = utils_get_locale_from_utf8(p->file_name);
	g_key_file_load_from_file(config, filename, G_KEY_FILE_NONE, NULL);

	foreach_slist(node, stash_groups)
		stash_group_save_to_key_file(node->data, config);

	g_key_file_set_string(config, "project", "name", p->name);
	g_key_file_set_string(config, "project", "base_path", p->base_path);

	if (p->description)
		g_key_file_set_string(config, "project", "description", p->description);
	if (p->file_patterns)
		g_key_file_set_string_list(config, "project", "file_patterns",
			(const gchar**) p->file_patterns, g_strv_length(p->file_patterns));

	// editor settings
	g_key_file_set_integer(config, "long line marker", "long_line_behaviour", p->priv->long_line_behaviour);
	g_key_file_set_integer(config, "long line marker", "long_line_column", p->priv->long_line_column);

	/* store the session files into the project too */
	configuration_save_session_files(config);
	build_save_menu(config, (gpointer)p, GEANY_BCS_PROJ);
	g_signal_emit_by_name(geany_object, "project-save", config);
	/* write the file */
	data = g_key_file_to_data(config, NULL, NULL);
	ret = (utils_write_file(filename, data) == 0);

	g_free(data);
	g_free(filename);
	g_key_file_free(config);

	return ret;
}


/** Forces the project file rewrite and emission of the project-save signal. Plugins
 * can use this function to save additional project data outside the project dialog.
 *
 *  @since 1.25
 */
GEANY_API_SYMBOL
void project_write_config(void)
{
	if (!write_config())
		SHOW_ERR(_("Project file could not be written"));
}


/* Constructs the project's base path which is used for "Make all" and "Execute".
 * The result is an absolute string in UTF-8 encoding which is either the same as
 * base path if it is absolute or it is built out of project file name's dir and base_path.
 * If there is no project or project's base_path is invalid, NULL will be returned.
 * The returned string should be freed when no longer needed. */
gchar *project_get_base_path(void)
{
	GeanyProject *project = app->project;

	if (project && !EMPTY(project->base_path))
	{
		if (g_path_is_absolute(project->base_path))
			return g_strdup(project->base_path);
		else
		{	/* build base_path out of project file name's dir and base_path */
			gchar *path;
			gchar *dir = g_path_get_dirname(project->file_name);

			if (utils_str_equal(project->base_path, "./"))
				return dir;

			path = g_build_filename(dir, project->base_path, NULL);
			g_free(dir);
			return path;
		}
	}
	return NULL;
}


/* This is to save project-related global settings, NOT project file settings. */
void project_save_prefs(GKeyFile *config)
{
	GeanyProject *project = app->project;

	if (cl_options.load_session)
	{
		const gchar *utf8_filename = (project == NULL) ? "" : project->file_name;

		g_key_file_set_string(config, "project", "session_file", utf8_filename);
	}
	g_key_file_set_string(config, "project", "project_file_path",
		FALLBACK(local_prefs.project_file_path, ""));
}


void project_load_prefs(GKeyFile *config)
{
	if (cl_options.load_session)
	{
		g_return_if_fail(project_prefs.session_file == NULL);
		project_prefs.session_file = utils_get_setting_string(config, "project",
			"session_file", "");
	}
	local_prefs.project_file_path = utils_get_setting_string(config, "project",
		"project_file_path", NULL);
	if (local_prefs.project_file_path == NULL)
	{
		local_prefs.project_file_path = g_build_filename(g_get_home_dir(), PROJECT_DIR, NULL);
	}
}


/* Initialize project-related preferences in the Preferences dialog. */
void project_setup_prefs(void)
{
	GtkWidget *path_entry = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_entry");
	GtkWidget *path_btn = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_button");
	static gboolean callback_setup = FALSE;

	g_return_if_fail(local_prefs.project_file_path != NULL);

	gtk_entry_set_text(GTK_ENTRY(path_entry), local_prefs.project_file_path);
	if (! callback_setup)
	{	/* connect the callback only once */
		callback_setup = TRUE;
		ui_setup_open_button_callback(path_btn, NULL,
			GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_ENTRY(path_entry));
	}
}


/* Update project-related preferences after using the Preferences dialog. */
void project_apply_prefs(void)
{
	GtkWidget *path_entry = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_entry");
	const gchar *str;

	str = gtk_entry_get_text(GTK_ENTRY(path_entry));
	SETPTR(local_prefs.project_file_path, g_strdup(str));
}


static void add_stash_group(StashGroup *group, gboolean apply_defaults)
{
	GKeyFile *kf;

	stash_groups = g_slist_prepend(stash_groups, group);
	if (!apply_defaults)
		return;

	kf = g_key_file_new();
	stash_group_load_from_key_file(group, kf);
	g_key_file_free(kf);
}


static void init_stash_prefs(void)
{
	StashGroup *group;

	group = stash_group_new("indentation");
	/* copy global defaults */
	indentation = *editor_get_indent_prefs(NULL);
	stash_group_set_use_defaults(group, FALSE);
	add_stash_group(group, FALSE);

	stash_group_add_spin_button_integer(group, &indentation.width,
		"indent_width", 4, "spin_indent_width_project");
	stash_group_add_radio_buttons(group, (gint*)(gpointer)&indentation.type,
		"indent_type", GEANY_INDENT_TYPE_TABS,
		"radio_indent_spaces_project", GEANY_INDENT_TYPE_SPACES,
		"radio_indent_tabs_project", GEANY_INDENT_TYPE_TABS,
		"radio_indent_both_project", GEANY_INDENT_TYPE_BOTH,
		NULL);
	/* This is a 'hidden' pref for backwards-compatibility */
	stash_group_add_integer(group, &indentation.hard_tab_width,
		"indent_hard_tab_width", 8);
	stash_group_add_toggle_button(group, &indentation.detect_type,
		"detect_indent", FALSE, "check_detect_indent_type_project");
	stash_group_add_toggle_button(group, &indentation.detect_width,
		"detect_indent_width", FALSE, "check_detect_indent_width_project");
	stash_group_add_combo_box(group, (gint*)(gpointer)&indentation.auto_indent_mode,
		"indent_mode", GEANY_AUTOINDENT_CURRENTCHARS, "combo_auto_indent_mode_project");

	group = stash_group_new("file_prefs");
	stash_group_add_toggle_button(group, &priv.final_new_line,
		"final_new_line", file_prefs.final_new_line, "check_new_line1");
	stash_group_add_toggle_button(group, &priv.ensure_convert_new_lines,
		"ensure_convert_new_lines", file_prefs.ensure_convert_new_lines, "check_ensure_convert_new_lines1");
	stash_group_add_toggle_button(group, &priv.strip_trailing_spaces,
		"strip_trailing_spaces", file_prefs.strip_trailing_spaces, "check_trailing_spaces1");
	stash_group_add_toggle_button(group, &priv.replace_tabs,
		"replace_tabs", file_prefs.replace_tabs, "check_replace_tabs1");
	add_stash_group(group, TRUE);

	group = stash_group_new("editor");
	stash_group_add_toggle_button(group, &priv.line_wrapping,
		"line_wrapping", editor_prefs.line_wrapping, "check_line_wrapping1");
	stash_group_add_spin_button_integer(group, &priv.line_break_column,
		"line_break_column", editor_prefs.line_break_column, "spin_line_break1");
	stash_group_add_toggle_button(group, &priv.auto_continue_multiline,
		"auto_continue_multiline", editor_prefs.auto_continue_multiline,
		"check_auto_multiline1");
	add_stash_group(group, TRUE);
}


#define COPY_PREF(dest, prefname)\
	(dest.prefname = priv.prefname)

const GeanyFilePrefs *project_get_file_prefs(void)
{
	static GeanyFilePrefs fp;

	if (!app->project)
		return &file_prefs;

	fp = file_prefs;
	COPY_PREF(fp, final_new_line);
	COPY_PREF(fp, ensure_convert_new_lines);
	COPY_PREF(fp, strip_trailing_spaces);
	COPY_PREF(fp, replace_tabs);
	return &fp;
}


void project_init(void)
{
}


void project_finalize(void)
{
}
