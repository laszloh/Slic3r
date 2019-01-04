#include "GUI_ObjectList.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "OptionsGroup.hpp"
#include "PresetBundle.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Model.hpp"
#include "LambdaObjectDialog.hpp"
#include "GLCanvas3D.hpp"

#include <boost/algorithm/string.hpp>
#include "slic3r/Utils/FixModelByWin10.hpp"

namespace Slic3r
{
namespace GUI
{

wxDEFINE_EVENT(EVT_OBJ_LIST_OBJECT_SELECT, SimpleEvent);

ObjectList::ObjectList(wxWindow* parent) :
    wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_MULTIPLE),
    m_parent(parent)
{
    // Fill CATEGORY_ICON
    {
        // ptFFF
		CATEGORY_ICON[L("Layers and Perimeters")]	= wxBitmap(from_u8(var("layers.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Infill")]					= wxBitmap(from_u8(var("infill.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Support material")]		= wxBitmap(from_u8(var("building.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Speed")]					= wxBitmap(from_u8(var("time.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Extruders")]				= wxBitmap(from_u8(var("funnel.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Extrusion Width")]			= wxBitmap(from_u8(var("funnel.png")), wxBITMAP_TYPE_PNG);
// 		CATEGORY_ICON[L("Skirt and brim")]			= wxBitmap(from_u8(var("box.png")), wxBITMAP_TYPE_PNG);
// 		CATEGORY_ICON[L("Speed > Acceleration")]	= wxBitmap(from_u8(var("time.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Advanced")]				= wxBitmap(from_u8(var("wand.png")), wxBITMAP_TYPE_PNG);
		// ptSLA
		CATEGORY_ICON[L("Supports")]				= wxBitmap(from_u8(var("building.png")), wxBITMAP_TYPE_PNG);
		CATEGORY_ICON[L("Pad")]				        = wxBitmap(from_u8(var("brick.png")), wxBITMAP_TYPE_PNG);
    }

    // create control
    create_objects_ctrl();

    init_icons();

    // create popup menus for object and part
    create_object_popupmenu(&m_menu_object);
    create_part_popupmenu(&m_menu_part);
    create_sla_object_popupmenu(&m_menu_sla_object);

    // describe control behavior 
    Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxEvent& event) {
        selection_changed();
#ifndef __WXMSW__
        set_tooltip_for_item(get_mouse_position_in_control());
#endif //__WXMSW__        
    });

//     Bind(wxEVT_CHAR, [this](wxKeyEvent& event) { key_event(event); }); // doesn't work on OSX

#ifdef __WXMSW__
    GetMainWindow()->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        set_tooltip_for_item(/*event.GetPosition()*/get_mouse_position_in_control());
        event.Skip();
    });
#endif //__WXMSW__

    Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,  &ObjectList::OnContextMenu,     this);

    Bind(wxEVT_DATAVIEW_ITEM_BEGIN_DRAG,    &ObjectList::OnBeginDrag,       this);
    Bind(wxEVT_DATAVIEW_ITEM_DROP_POSSIBLE, &ObjectList::OnDropPossible,    this);
    Bind(wxEVT_DATAVIEW_ITEM_DROP,          &ObjectList::OnDrop,            this);

    Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE,  &ObjectList::OnEditingDone,     this);

    Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, &ObjectList::ItemValueChanged,  this);

    Bind(wxCUSTOMEVT_LAST_VOLUME_IS_DELETED, [this](wxCommandEvent& e)   { last_volume_is_deleted(e.GetInt()); });

#ifdef __WXOSX__
    Bind(wxEVT_KEY_DOWN, &ObjectList::OnChar, this);
#endif //__WXOSX__
}

ObjectList::~ObjectList()
{
    if (m_default_config) 
        delete m_default_config;
}

void ObjectList::create_objects_ctrl()
{
    // temporary workaround for the correct behavior of the Scrolled sidebar panel:
    // 1. set a height of the list to some big value 
    // 2. change it to the normal min value (200) after first whole App updating/layouting
    SetMinSize(wxSize(-1, 1500));   // #ys_FIXME 

    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(this, 1, wxGROW | wxLEFT, 20);

    m_objects_model = new PrusaObjectDataViewModel;
    AssociateModel(m_objects_model);
    m_objects_model->SetAssociatedControl(this);
#if wxUSE_DRAG_AND_DROP && wxUSE_UNICODE
    EnableDragSource(wxDF_UNICODETEXT);
    EnableDropTarget(wxDF_UNICODETEXT);
#endif // wxUSE_DRAG_AND_DROP && wxUSE_UNICODE

    // column 0(Icon+Text) of the view control: 
    // And Icon can be consisting of several bitmaps
    AppendColumn(new wxDataViewColumn(_(L("Name")), new PrusaBitmapTextRenderer(),
        0, 200, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE));

    // column 1 of the view control:
    AppendColumn(create_objects_list_extruder_column(4));

    // column 2 of the view control:
    AppendBitmapColumn(" ", 2, wxDATAVIEW_CELL_INERT, 25,
        wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE);
}

void ObjectList::set_tooltip_for_item(const wxPoint& pt)
{
    wxDataViewItem item;
    wxDataViewColumn* col;
    HitTest(pt, item, col);
    if (!item) return;

    if (col->GetTitle() == " " && GetSelectedItemsCount()<2)
        GetMainWindow()->SetToolTip(_(L("Right button click the icon to change the object settings")));
    else if (col->GetTitle() == _("Name") &&
        m_objects_model->GetBitmap(item).GetRefData() == m_bmp_manifold_warning.GetRefData()) {
        int obj_idx = m_objects_model->GetIdByItem(item);
        auto& stats = (*m_objects)[obj_idx]->volumes[0]->mesh.stl.stats;
        int errors = stats.degenerate_facets + stats.edges_fixed + stats.facets_removed +
            stats.facets_added + stats.facets_reversed + stats.backwards_edges;

        wxString tooltip = wxString::Format(_(L("Auto-repaired (%d errors):\n")), errors);

        std::map<std::string, int> error_msg;
        error_msg[L("degenerate facets")] = stats.degenerate_facets;
        error_msg[L("edges fixed")] = stats.edges_fixed;
        error_msg[L("facets removed")] = stats.facets_removed;
        error_msg[L("facets added")] = stats.facets_added;
        error_msg[L("facets reversed")] = stats.facets_reversed;
        error_msg[L("backwards edges")] = stats.backwards_edges;

        for (auto error : error_msg)
        {
            if (error.second > 0)
                tooltip += wxString::Format(_("\t%d %s\n"), error.second, error.first);
        }
// OR
//             tooltip += wxString::Format(_(L("%d degenerate facets, %d edges fixed, %d facets removed, "
//                                             "%d facets added, %d facets reversed, %d backwards edges")),
//                                             stats.degenerate_facets, stats.edges_fixed, stats.facets_removed,
//                                             stats.facets_added, stats.facets_reversed, stats.backwards_edges);

        if (is_windows10())
            tooltip += _(L("Right button click the icon to fix STL through Netfabb"));

        GetMainWindow()->SetToolTip(tooltip);
    }
    else
        GetMainWindow()->SetToolTip(""); // hide tooltip
}

wxPoint ObjectList::get_mouse_position_in_control()
{
    const wxPoint& pt = wxGetMousePosition();
//     wxWindow* win = GetMainWindow();
//     wxPoint screen_pos = win->GetScreenPosition();
    return wxPoint(pt.x - /*win->*/GetScreenPosition().x, pt.y - /*win->*/GetScreenPosition().y);
}

int ObjectList::get_selected_obj_idx() const
{
    if (GetSelectedItemsCount() == 1)
        return m_objects_model->GetIdByItem(m_objects_model->GetTopParent(GetSelection()));

    return -1;
}

wxDataViewColumn* ObjectList::create_objects_list_extruder_column(int extruders_count)
{
    wxArrayString choices;
    choices.Add("default");
    for (int i = 1; i <= extruders_count; ++i)
        choices.Add(wxString::Format("%d", i));
    wxDataViewChoiceRenderer *c =
        new wxDataViewChoiceRenderer(choices, wxDATAVIEW_CELL_EDITABLE, wxALIGN_CENTER_HORIZONTAL);
    wxDataViewColumn* column = new wxDataViewColumn(_(L("Extruder")), c, 1, 80, wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE);
    return column;
}

void ObjectList::update_extruder_values_for_items(const int max_extruder)
{
    for (int i = 0; i < m_objects->size(); ++i)
    {
        wxDataViewItem item = m_objects_model->GetItemById(i);
        if (!item) continue;
            
        auto object = (*m_objects)[i];
        wxString extruder;
        if (!object->config.has("extruder") ||
            object->config.option<ConfigOptionInt>("extruder")->value > max_extruder)
            extruder = "default";
        else
            extruder = wxString::Format("%d", object->config.option<ConfigOptionInt>("extruder")->value);

        m_objects_model->SetValue(extruder, item, 1);

        if (object->volumes.size() > 1) {
            for (auto id = 0; id < object->volumes.size(); id++) {
                item = m_objects_model->GetItemByVolumeId(i, id);
                if (!item) continue;
                if (!object->volumes[id]->config.has("extruder") ||
                    object->volumes[id]->config.option<ConfigOptionInt>("extruder")->value > max_extruder)
                    extruder = "default";
                else
                    extruder = wxString::Format("%d", object->volumes[id]->config.option<ConfigOptionInt>("extruder")->value); 

                m_objects_model->SetValue(extruder, item, 1);
            }
        }
    }
}

void ObjectList::update_objects_list_extruder_column(int extruders_count)
{
    if (!this) return; // #ys_FIXME
    if (wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        extruders_count = 1;

    wxDataViewChoiceRenderer* ch_render = dynamic_cast<wxDataViewChoiceRenderer*>(GetColumn(1)->GetRenderer());
    if (ch_render->GetChoices().GetCount() - 1 == extruders_count)
        return;
    
    m_prevent_update_extruder_in_config = true;

    if (m_objects && extruders_count > 1)
        update_extruder_values_for_items(extruders_count);

    // delete old 2nd column
    DeleteColumn(GetColumn(1));
    // insert new created 3rd column
    InsertColumn(1, create_objects_list_extruder_column(extruders_count));
    // set show/hide for this column 
    set_extruder_column_hidden(extruders_count <= 1);
    //a workaround for a wrong last column width updating under OSX 
    GetColumn(2)->SetWidth(25);

    m_prevent_update_extruder_in_config = false;
}

void ObjectList::set_extruder_column_hidden(const bool hide) const
{
    GetColumn(1)->SetHidden(hide);
}

void ObjectList::update_extruder_in_config(const wxDataViewItem& item)
{
    if (m_prevent_update_extruder_in_config)
        return;
    if (m_objects_model->GetParent(item) == wxDataViewItem(0)) {
        const int obj_idx = m_objects_model->GetIdByItem(item);
        m_config = &(*m_objects)[obj_idx]->config;
    }
    else {
        const int obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetParent(item));
        const int volume_id = m_objects_model->GetVolumeIdByItem(item);
        if (obj_idx < 0 || volume_id < 0)
            return;
        m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
    }

    wxVariant variant;
    m_objects_model->GetValue(variant, item, 1);
    const wxString selection = variant.GetString();

    if (!m_config || selection.empty())
        return;

    const int extruder = selection.size() > 1 ? 0 : atoi(selection.c_str());
    m_config->set_key_value("extruder", new ConfigOptionInt(extruder));

    // update scene
    wxGetApp().plater()->update();
}

void ObjectList::update_name_in_model(const wxDataViewItem& item)
{
    const int obj_idx = m_objects_model->GetObjectIdByItem(item);
    if (obj_idx < 0) return;

    if (m_objects_model->GetParent(item) == wxDataViewItem(0)) {
        (*m_objects)[obj_idx]->name = m_objects_model->GetName(item).ToStdString();
        return;
    }

    const int volume_id = m_objects_model->GetVolumeIdByItem(item);
    if (volume_id < 0) return;
    (*m_objects)[obj_idx]->volumes[volume_id]->name = m_objects_model->GetName(item).ToStdString();
}

void ObjectList::init_icons()
{
    m_bmp_modifiermesh      = wxBitmap(from_u8(var("lambda.png")), wxBITMAP_TYPE_PNG);//(Slic3r::var("plugin.png")), wxBITMAP_TYPE_PNG);
    m_bmp_solidmesh         = wxBitmap(from_u8(var("object.png")), wxBITMAP_TYPE_PNG);//(Slic3r::var("package.png")), wxBITMAP_TYPE_PNG);

    m_bmp_support_enforcer  = wxBitmap(from_u8(var("support_enforcer_.png")), wxBITMAP_TYPE_PNG);
    m_bmp_support_blocker   = wxBitmap(from_u8(var("support_blocker_.png")), wxBITMAP_TYPE_PNG);

    m_bmp_vector.reserve(4); // bitmaps for different types of parts 
    m_bmp_vector.push_back(&m_bmp_solidmesh);         // Add part
    m_bmp_vector.push_back(&m_bmp_modifiermesh);      // Add modifier
    m_bmp_vector.push_back(&m_bmp_support_enforcer);  // Add support enforcer
    m_bmp_vector.push_back(&m_bmp_support_blocker);   // Add support blocker
    m_objects_model->SetVolumeBitmaps(m_bmp_vector);

    // init icon for manifold warning
    m_bmp_manifold_warning  = wxBitmap(from_u8(var("exclamation_mark_.png")), wxBITMAP_TYPE_PNG);//(Slic3r::var("error.png")), wxBITMAP_TYPE_PNG);

    // init bitmap for "Split to sub-objects" context menu
    m_bmp_split             = wxBitmap(from_u8(var("split.png")), wxBITMAP_TYPE_PNG);

    // init bitmap for "Add Settings" context menu
    m_bmp_cog               = wxBitmap(from_u8(var("cog.png")), wxBITMAP_TYPE_PNG);
}


void ObjectList::selection_changed()
{
    if (m_prevent_list_events) return;

    fix_multiselection_conflicts();

    // update object selection on Plater
    update_selections_on_canvas();

    // to update the toolbar and info sizer
    if (!GetSelection() || m_objects_model->GetItemType(GetSelection()) == itObject) {
        auto event = SimpleEvent(EVT_OBJ_LIST_OBJECT_SELECT);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }

    part_selection_changed();
}

void ObjectList::OnChar(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_BACK){
        remove();
    }
    else if (wxGetKeyState(wxKeyCode('A')) && wxGetKeyState(WXK_SHIFT))
        select_item_all_children();

    event.Skip();
}

void ObjectList::OnContextMenu(wxDataViewEvent&)
{
    wxDataViewItem item;
    wxDataViewColumn* col;
    const wxPoint pt = get_mouse_position_in_control();
    HitTest(pt, item, col);
    if (!item)
#ifdef __WXOSX__ // #ys_FIXME temporary workaround for OSX 
        // after Yosemite OS X version, HitTest return undefined item
        item = GetSelection();
    if (item)
        show_context_menu();
    else
        printf("undefined item\n");
    return;
#else
        return;
#endif // __WXOSX__
    const wxString title = col->GetTitle();

    if (title == " ")
        show_context_menu();
    else if (title == _("Name") && pt.x >15 &&
             m_objects_model->GetBitmap(item).GetRefData() == m_bmp_manifold_warning.GetRefData())
    {
        if (is_windows10()) {
            const auto obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetTopParent(item));
            wxGetApp().plater()->fix_through_netfabb(obj_idx);
        }
    }
#ifndef __WXMSW__
    GetMainWindow()->SetToolTip(""); // hide tooltip
#endif //__WXMSW__
}

void ObjectList::show_context_menu()
{
    const auto item = GetSelection();
    if (item)
    {
        if (!(m_objects_model->GetItemType(item) & (itObject | itVolume)))
            return;
        wxMenu* menu = m_objects_model->GetParent(item) != wxDataViewItem(0) ? &m_menu_part :
                       wxGetApp().plater()->printer_technology() == ptFFF ? &m_menu_object : &m_menu_sla_object;

        append_menu_item_settings(menu);

        wxGetApp().plater()->PopupMenu(menu);

        wxGetApp().plater()->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) {
            evt.Enable(is_splittable()); }, m_menu_item_split->GetId());

        wxGetApp().plater()->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) {
            evt.Enable(is_splittable()); }, m_menu_item_split_part->GetId());
    }
}


void ObjectList::key_event(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_TAB)
        Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    else if (event.GetKeyCode() == WXK_DELETE
#ifdef __WXOSX__
        || event.GetKeyCode() == WXK_BACK
#endif //__WXOSX__
        ) {
        printf("WXK_BACK\n");
        remove();
    }
    else if (wxGetKeyState(wxKeyCode('A')) && wxGetKeyState(WXK_SHIFT))
        select_item_all_children();
    else
        event.Skip();
}

void ObjectList::OnBeginDrag(wxDataViewEvent &event)
{
    const wxDataViewItem item(event.GetItem());

    // only allow drags for item, not containers
    if (multiple_selection() || GetSelection()!=item || 
        m_objects_model->GetParent(item) == wxDataViewItem(0) ||
        m_objects_model->GetItemType(item) != itVolume ) {
        event.Veto();
        return;
    }

    m_dragged_data.init(m_objects_model->GetObjectIdByItem(item), m_objects_model->GetVolumeIdByItem(item));

    /* Under MSW or OSX, DnD moves an item to the place of another selected item
    * But under GTK, DnD moves an item between another two items.
    * And as a result - call EVT_CHANGE_SELECTION to unselect all items.
    * To prevent such behavior use m_prevent_list_events
    **/
    m_prevent_list_events = true;//it's needed for GTK

    /* Under GTK, DnD requires to the wxTextDataObject been initialized with some valid value,
     * so set some nonempty string
     */
    wxTextDataObject* obj = new wxTextDataObject;
    obj->SetText("Some text");//it's needed for GTK

    event.SetDataObject(obj);
    event.SetDragFlags(wxDrag_DefaultMove); // allows both copy and move;
}

void ObjectList::OnDropPossible(wxDataViewEvent &event)
{
    wxDataViewItem item(event.GetItem());

    // only allow drags for item or background, not containers
    if (!item.IsOk() ||
        m_objects_model->GetParent(item) == wxDataViewItem(0) || 
        m_objects_model->GetItemType(item) != itVolume ||
        m_dragged_data.obj_idx() != m_objects_model->GetObjectIdByItem(item))
        event.Veto();
}

void ObjectList::OnDrop(wxDataViewEvent &event)
{
    wxDataViewItem item(event.GetItem());

    if (!item.IsOk() || m_objects_model->GetParent(item) == wxDataViewItem(0) ||
                        m_objects_model->GetItemType(item) != itVolume ||
                        m_dragged_data.obj_idx() != m_objects_model->GetObjectIdByItem(item)) {
        event.Veto();
        m_dragged_data.clear();
        return;
    }

    const int from_volume_id = m_dragged_data.vol_idx();
    int to_volume_id = m_objects_model->GetVolumeIdByItem(item);

// It looks like a fixed in current version of the wxWidgets
// #ifdef __WXGTK__
//     /* Under GTK, DnD moves an item between another two items.
//     * And event.GetItem() return item, which is under "insertion line"
//     * So, if we move item down we should to decrease the to_volume_id value
//     **/
//     if (to_volume_id > from_volume_id) to_volume_id--;
// #endif // __WXGTK__

    auto& volumes = (*m_objects)[/*m_selected_object_id*/m_dragged_data.obj_idx()]->volumes;
    auto delta = to_volume_id < from_volume_id ? -1 : 1;
    int cnt = 0;
    for (int id = from_volume_id; cnt < abs(from_volume_id - to_volume_id); id += delta, cnt++)
        std::swap(volumes[id], volumes[id + delta]);

    select_item(m_objects_model->ReorganizeChildren(from_volume_id, to_volume_id,
                                                    m_objects_model->GetParent(item)));

    m_parts_changed = true;
    parts_changed(/*m_selected_object_id*/m_dragged_data.obj_idx());

    m_dragged_data.clear();
}


// Context Menu

std::vector<std::string> get_options(const bool is_part, const bool is_sla)
{
    if (is_sla) {
        SLAPrintObjectConfig full_sla_config;
        auto options = full_sla_config.keys();
        options.erase(find(options.begin(), options.end(), "layer_height"));
        return options;
    }

    PrintRegionConfig reg_config;
    auto options = reg_config.keys();
    if (!is_part) {
        PrintObjectConfig obj_config;
        std::vector<std::string> obj_options = obj_config.keys();
        options.insert(options.end(), obj_options.begin(), obj_options.end());
    }
    return options;
}

std::vector<std::string> get_options(const bool is_part)
{
    return get_options(is_part, wxGetApp().plater()->printer_technology() == ptSLA);
}

//				  category ->		vector 			 ( option	;  label )
typedef std::map< std::string, std::vector< std::pair<std::string, std::string> > > settings_menu_hierarchy;
void get_options_menu(settings_menu_hierarchy& settings_menu, const bool is_part, const bool is_sla)
{
    auto options = get_options(is_part, is_sla);

    auto extruders_cnt = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA ? 1 :
        wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    DynamicPrintConfig config;
    for (auto& option : options)
    {
        auto const opt = config.def()->get(option);
        auto category = opt->category;
        if (category.empty() ||
            (category == "Extruders" && extruders_cnt == 1)) continue;

        const std::string& label = opt->label.empty() ? opt->full_label : 
                                   opt->full_label.empty() ? opt->label :
                                   opt->full_label + " " + opt->label;;
        std::pair<std::string, std::string> option_label(option, label);
        std::vector< std::pair<std::string, std::string> > new_category;
        auto& cat_opt_label = settings_menu.find(category) == settings_menu.end() ? new_category : settings_menu.at(category);
        cat_opt_label.push_back(option_label);
        if (cat_opt_label.size() == 1)
            settings_menu[category] = cat_opt_label;
    }
}

void get_options_menu(settings_menu_hierarchy& settings_menu, const bool is_part)
{
    get_options_menu(settings_menu, is_part, wxGetApp().plater()->printer_technology() == ptSLA);
}

void ObjectList::get_settings_choice(const wxString& category_name)
{
    wxArrayString names;
    wxArrayInt selections;

    settings_menu_hierarchy settings_menu;
    const bool is_part = m_objects_model->GetParent(GetSelection()) != wxDataViewItem(0);
    get_options_menu(settings_menu, is_part);
    std::vector< std::pair<std::string, std::string> > *settings_list = nullptr;

    auto opt_keys = m_config->keys();

    for (auto& cat : settings_menu)
    {
        if (_(cat.first) == category_name) {
            int sel = 0;
            for (auto& pair : cat.second) {
                names.Add(_(pair.second));
                if (find(opt_keys.begin(), opt_keys.end(), pair.first) != opt_keys.end())
                    selections.Add(sel);
                sel++;
            }
            settings_list = &cat.second;
            break;
        }
    }

    if (!settings_list)
        return;

    if (wxGetSelectedChoices(selections, _(L("Select showing settings")), category_name, names) == -1)
        return;

    std::vector <std::string> selected_options;
    for (auto sel : selections)
        selected_options.push_back((*settings_list)[sel].first);

    for (auto& setting : (*settings_list))
    {
        auto& opt_key = setting.first;
        if (find(opt_keys.begin(), opt_keys.end(), opt_key) != opt_keys.end() &&
            find(selected_options.begin(), selected_options.end(), opt_key) == selected_options.end())
            m_config->erase(opt_key);

        if (find(opt_keys.begin(), opt_keys.end(), opt_key) == opt_keys.end() &&
            find(selected_options.begin(), selected_options.end(), opt_key) != selected_options.end())
            m_config->set_key_value(opt_key, m_default_config->option(opt_key)->clone());
    }


    // Add settings item for object
    const auto item = GetSelection();
    if (item) {
        auto settings_item = m_objects_model->GetSettingsItem(item);

        // if settings_item is now false, the settings item is either not created yet, OR it is already selected
        // we must first check if the latter is not the case before adding it:
        if (!settings_item) {
            PrusaObjectDataViewModelNode* selected_node = (PrusaObjectDataViewModelNode*)item.GetID();
            if (selected_node && selected_node->m_type == itSettings)
                settings_item = item;
        }
        
        select_item(settings_item ? settings_item :
            m_objects_model->AddSettingsChild(item));
    }
    else {
        auto panel = wxGetApp().sidebar().scrolled_panel();
        panel->Freeze();
        wxGetApp().obj_settings()->UpdateAndShow(true);//obj_manipul()->update_settings_list();
        panel->Thaw();
    }
}

void ObjectList::append_menu_item_add_generic(wxMenuItem* menu, const int type) {
    auto sub_menu = new wxMenu;

    append_menu_item(sub_menu, wxID_ANY, _(L("Load")) + " " + dots, "",
        [this, type](wxCommandEvent&) { load_subobject(type); }, "", menu->GetMenu());
    sub_menu->AppendSeparator();

    std::vector<std::string> menu_items = { L("Box"), L("Cylinder"), L("Sphere"), L("Slab") };
    for (auto& item : menu_items) {
        append_menu_item(sub_menu, wxID_ANY, _(item), "",
            [this, type, item](wxCommandEvent&) { load_generic_subobject(_(item).ToStdString(), type); }, "", menu->GetMenu());
    }

    menu->SetSubMenu(sub_menu);
}

void ObjectList::append_menu_items_add_volume(wxMenu* menu)
{
    // Note: id accords to type of the sub-object, so sequence of the menu items is important
    std::vector<std::string> menu_object_types_items = {L("Add part"),              // ~ModelVolume::MODEL_PART
                                                        L("Add modifier"),          // ~ModelVolume::PARAMETER_MODIFIER
                                                        L("Add support enforcer"),  // ~ModelVolume::SUPPORT_ENFORCER
                                                        L("Add support blocker") }; // ~ModelVolume::SUPPORT_BLOCKER

    // Update "add" items(delete old & create new)  settings popupmenu
    for (auto& item : menu_object_types_items){
        const auto settings_id = menu->FindItem(_(item));
        if (settings_id != wxNOT_FOUND)
            menu->Destroy(settings_id);
    }

    if (wxGetApp().get_view_mode() == ConfigMenuModeSimple)
    {
        append_menu_item(menu, wxID_ANY, _(L("Add part")), "",
            [this](wxCommandEvent&) { load_subobject(ModelVolume::MODEL_PART); }, *m_bmp_vector[ModelVolume::MODEL_PART]);
        append_menu_item(menu, wxID_ANY, _(L("Add support enforcer")), "",
            [this](wxCommandEvent&) { load_generic_subobject(_(L("Box")).ToStdString(), ModelVolume::SUPPORT_ENFORCER); }, 
            *m_bmp_vector[ModelVolume::SUPPORT_ENFORCER]);
        append_menu_item(menu, wxID_ANY, _(L("Add support blocker")), "",
            [this](wxCommandEvent&) { load_generic_subobject(_(L("Box")).ToStdString(), ModelVolume::SUPPORT_BLOCKER); }, 
            *m_bmp_vector[ModelVolume::SUPPORT_BLOCKER]);

        return;
    }

    for (int type = 0; type < menu_object_types_items.size(); type++) 
    {
        auto& item = menu_object_types_items[type];

        auto menu_item = new wxMenuItem(menu, wxID_ANY, _(item));
        menu_item->SetBitmap(*m_bmp_vector[type]);
        append_menu_item_add_generic(menu_item, type);

        menu->Append(menu_item);
    }
}

wxMenuItem* ObjectList::append_menu_item_split(wxMenu* menu) 
{
    return append_menu_item(menu, wxID_ANY, _(L("Split to parts")), "",
        [this](wxCommandEvent&) { split(); }, m_bmp_split, menu);
}

wxMenuItem* ObjectList::append_menu_item_settings(wxMenu* menu) 
{
    // Update (delete old & create new)  settings popupmenu
    const auto settings_id = menu->FindItem(_("Add settings"));
    if (settings_id != wxNOT_FOUND)
        menu->Destroy(settings_id);

    if (wxGetApp().get_view_mode() == ConfigMenuModeSimple)
        return nullptr;

    auto  menu_item = new wxMenuItem(menu, wxID_ANY, _(L("Add settings")));
    menu_item->SetBitmap(m_bmp_cog);

    const auto sel_vol = get_selected_model_volume();
    if (sel_vol && sel_vol->type() >= ModelVolume::SUPPORT_ENFORCER)
        menu_item->Enable(false);
    else
        menu_item->SetSubMenu(create_settings_popupmenu(menu));

    return menu->Append(menu_item);
}

wxMenuItem* ObjectList::append_menu_item_change_type(wxMenu* menu)
{
    return append_menu_item(menu, wxID_ANY, _(L("Change type")), "",
        [this](wxCommandEvent&) { change_part_type(); }, "", menu);

}

void ObjectList::create_object_popupmenu(wxMenu *menu)
{
    append_menu_items_add_volume(menu);

    // Split object to parts
    menu->AppendSeparator();
    m_menu_item_split = append_menu_item_split(menu);

    // Settings
    menu->AppendSeparator();
}

void ObjectList::create_sla_object_popupmenu(wxMenu *menu)
{
}

void ObjectList::create_part_popupmenu(wxMenu *menu)
{
    m_menu_item_split_part = append_menu_item_split(menu);

    // Append change part type
    menu->AppendSeparator();
    append_menu_item_change_type(menu);

    // Append settings popupmenu
    menu->AppendSeparator();
}

wxMenu* ObjectList::create_settings_popupmenu(wxMenu *parent_menu)
{
    wxMenu *menu = new wxMenu;

    settings_menu_hierarchy settings_menu;
    const bool is_part = m_objects_model->GetParent(GetSelection()) != wxDataViewItem(0);
    get_options_menu(settings_menu, is_part);

    for (auto cat : settings_menu) {
        append_menu_item(menu, wxID_ANY, _(cat.first), "",
                        [menu, this](wxCommandEvent& event) { get_settings_choice(menu->GetLabel(event.GetId())); }, 
                        CATEGORY_ICON.find(cat.first) == CATEGORY_ICON.end() ? wxNullBitmap : CATEGORY_ICON.at(cat.first), parent_menu); 
    }

    return menu;
}

void ObjectList::update_opt_keys(t_config_option_keys& opt_keys)
{
    auto full_current_opts = get_options(false);
    for (int i = opt_keys.size()-1; i >= 0; --i)
        if (find(full_current_opts.begin(), full_current_opts.end(), opt_keys[i]) == full_current_opts.end())
            opt_keys.erase(opt_keys.begin() + i);
}

void ObjectList::load_subobject(int type)
{
    auto item = GetSelection();
    if (!item || m_objects_model->GetParent(item) != wxDataViewItem(0))
        return;
    int obj_idx = m_objects_model->GetIdByItem(item);

    if (obj_idx < 0) return;
    wxArrayString part_names;
    load_part((*m_objects)[obj_idx], part_names, type);

    parts_changed(obj_idx);

    for (int i = 0; i < part_names.size(); ++i) {
        const wxDataViewItem sel_item = m_objects_model->AddVolumeChild(item, part_names.Item(i), type);

        if (i == part_names.size() - 1)
            select_item(sel_item);
    }
}

void ObjectList::load_part( ModelObject* model_object,
                            wxArrayString& part_names, 
                            int type)
{
    wxWindow* parent = wxGetApp().tab_panel()->GetPage(0);

    m_parts_changed = false;
    wxArrayString input_files;
    wxGetApp().import_model(parent, input_files);
    for (int i = 0; i < input_files.size(); ++i) {
        std::string input_file = input_files.Item(i).ToStdString();

        Model model;
        try {
            model = Model::read_from_file(input_file);
        }
        catch (std::exception &e) {
            auto msg = _(L("Error! ")) + input_file + " : " + e.what() + ".";
            show_error(parent, msg);
            exit(1);
        }

        for (auto object : model.objects) {
            Vec3d delta = Vec3d::Zero();
            if (model_object->origin_translation != Vec3d::Zero())
            {
                object->center_around_origin();
                delta = model_object->origin_translation - object->origin_translation;
            }
            for (auto volume : object->volumes) {
                volume->center_geometry();
                volume->translate(delta);
                auto new_volume = model_object->add_volume(*volume);
                new_volume->set_type(static_cast<ModelVolume::Type>(type));
                new_volume->name = boost::filesystem::path(input_file).filename().string();

                part_names.Add(new_volume->name);

                // set a default extruder value, since user can't add it manually
                new_volume->config.set_key_value("extruder", new ConfigOptionInt(0));

                m_parts_changed = true;
            }
        }
    }

}

void ObjectList::load_generic_subobject(const std::string& type_name, const int type)
{
    const auto obj_idx = get_selected_obj_idx();
    if (obj_idx < 0) return;

    const std::string name = "lambda-" + type_name;
    TriangleMesh mesh;

    auto& bed_shape = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("bed_shape")->values;
    const auto& sz = BoundingBoxf(bed_shape).size();
    const auto side = 0.1 * std::max(sz(0), sz(1));

    if (type_name == _("Box")) {
        mesh = make_cube(side, side, side);
        // box sets the base coordinate at 0, 0, move to center of plate
        mesh.translate(-side * 0.5, -side * 0.5, 0);
    }
    else if (type_name == _("Cylinder"))
        mesh = make_cylinder(0.5*side, side);
    else if (type_name == _("Sphere"))
        mesh = make_sphere(0.5*side, PI/18);
    else if (type_name == _("Slab")) {
        const auto& size = (*m_objects)[obj_idx]->bounding_box().size();
        mesh = make_cube(size(0)*1.5, size(1)*1.5, size(2)*0.5);
        // box sets the base coordinate at 0, 0, move to center of plate and move it up to initial_z
        mesh.translate(-size(0)*1.5 / 2.0, -size(1)*1.5 / 2.0, 0);
    }
    mesh.repair();
    
    auto new_volume = (*m_objects)[obj_idx]->add_volume(mesh);
    new_volume->set_type(static_cast<ModelVolume::Type>(type));

    new_volume->set_offset(Vec3d(0.0, 0.0, (*m_objects)[obj_idx]->origin_translation(2) - mesh.stl.stats.min(2)));
    new_volume->center_geometry();

    new_volume->name = name;
    // set a default extruder value, since user can't add it manually
    new_volume->config.set_key_value("extruder", new ConfigOptionInt(0));

    m_parts_changed = true;
    parts_changed(obj_idx);

    select_item(m_objects_model->AddVolumeChild(GetSelection(), name, type));
#ifndef __WXOSX__ //#ifdef __WXMSW__ // #ys_FIXME
    selection_changed();
#endif //no __WXOSX__ //__WXMSW__
}

void ObjectList::del_object(const int obj_idx)
{
    wxGetApp().plater()->delete_object_from_model(obj_idx);
}

// Delete subobject
void ObjectList::del_subobject_item(wxDataViewItem& item)
{
    if (!item) return;

    int obj_idx, idx;
    ItemType type;

    m_objects_model->GetItemInfo(item, type, obj_idx, idx);
    if (type == itUndef)
        return;

    if (type == itSettings)
        del_settings_from_config();
    else if (type == itInstanceRoot && obj_idx != -1)
        del_instances_from_object(obj_idx);
    else if (idx == -1)
        return;
    else if (!del_subobject_from_object(obj_idx, idx, type))
        return;

    m_objects_model->Delete(item);
}

void ObjectList::del_settings_from_config()
{
    auto opt_keys = m_config->keys();
    if (opt_keys.size() == 1 && opt_keys[0] == "extruder")
        return;
    int extruder = -1;
    if (m_config->has("extruder"))
        extruder = m_config->option<ConfigOptionInt>("extruder")->value;

    m_config->clear();

    if (extruder >= 0)
        m_config->set_key_value("extruder", new ConfigOptionInt(extruder));
}

void ObjectList::del_instances_from_object(const int obj_idx)
{
    auto& instances = (*m_objects)[obj_idx]->instances;
    if (instances.size() <= 1)
        return;

    while ( instances.size()> 1)
        instances.pop_back();

    (*m_objects)[obj_idx]->invalidate_bounding_box(); // ? #ys_FIXME

    m_parts_changed = true;
    parts_changed(obj_idx);
}

bool ObjectList::del_subobject_from_object(const int obj_idx, const int idx, const int type)
{
	if (obj_idx == 1000)
		// Cannot delete a wipe tower.
		return false;

    if (type == itVolume) {
        const auto volume = (*m_objects)[obj_idx]->volumes[idx];

        // if user is deleting the last solid part, throw error
        int solid_cnt = 0;
        for (auto vol : (*m_objects)[obj_idx]->volumes)
            if (vol->is_model_part())
                ++solid_cnt;
        if (volume->is_model_part() && solid_cnt == 1) {
            Slic3r::GUI::show_error(nullptr, _(L("You can't delete the last solid part from object.")));
            return false;
        }

        (*m_objects)[obj_idx]->delete_volume(idx);
    }
    else if (type == itInstance) {
        if ((*m_objects)[obj_idx]->instances.size() == 1) {
            Slic3r::GUI::show_error(nullptr, _(L("You can't delete the last intance from object.")));
            return false;
        }
        (*m_objects)[obj_idx]->delete_instance(idx);
    }
    else
        return false;

    m_parts_changed = true;
    parts_changed(obj_idx);

    return true;
}

void ObjectList::split()
{
    const auto item = GetSelection();
    const int obj_idx = get_selected_obj_idx();
    if (!item || obj_idx < 0)
        return;

    ModelVolume* volume;
    if (!get_volume_by_item(item, volume)) return;
    DynamicPrintConfig&	config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
	const ConfigOption *nozzle_dmtrs_opt = config.option("nozzle_diameter", false);
	const auto nozzle_dmrs_cnt = (nozzle_dmtrs_opt == nullptr) ? size_t(1) : dynamic_cast<const ConfigOptionFloats*>(nozzle_dmtrs_opt)->values.size();
    if (volume->split(nozzle_dmrs_cnt) == 1) {
        wxMessageBox(_(L("The selected object couldn't be split because it contains only one part.")));
        return;
    }

    auto model_object = (*m_objects)[obj_idx];

    auto parent = m_objects_model->GetTopParent(item);
    if (parent)
        m_objects_model->DeleteVolumeChildren(parent);
    else
        parent = item;

    for (auto id = 0; id < model_object->volumes.size(); id++) {
        const auto vol_item = m_objects_model->AddVolumeChild(parent, model_object->volumes[id]->name,
                                            model_object->volumes[id]->is_modifier() ? 
                                                ModelVolume::PARAMETER_MODIFIER : ModelVolume::MODEL_PART,
                                            model_object->volumes[id]->config.has("extruder") ?
                                                model_object->volumes[id]->config.option<ConfigOptionInt>("extruder")->value : 0,
                                            false);
        // add settings to the part, if it has those
        auto opt_keys = model_object->volumes[id]->config.keys();
        if ( !(opt_keys.size() == 1 && opt_keys[0] == "extruder") ) {
            select_item(m_objects_model->AddSettingsChild(vol_item));
            Collapse(vol_item);
        }
    }

    if (parent == item)
        Expand(parent);

    m_parts_changed = true;
    parts_changed(obj_idx);
}

bool ObjectList::get_volume_by_item(const wxDataViewItem& item, ModelVolume*& volume)
{
    auto obj_idx = get_selected_obj_idx();
    if (!item || obj_idx < 0)
        return false;
    const auto volume_id = m_objects_model->GetVolumeIdByItem(item);
    const bool split_part = m_objects_model->GetItemType(item) == itVolume;

    // object is selected
    if (volume_id < 0) {
        if ( split_part || (*m_objects)[obj_idx]->volumes.size() > 1 ) 
            return false;
        volume = (*m_objects)[obj_idx]->volumes[0];
    }
    // volume is selected
    else
        volume = (*m_objects)[obj_idx]->volumes[volume_id];
    
    return true;
}

bool ObjectList::is_splittable()
{
    const wxDataViewItem item = GetSelection();
    if (!item) return false;

    ModelVolume* volume;
    if (!get_volume_by_item(item, volume) || !volume)
        return false;

    TriangleMeshPtrs meshptrs = volume->mesh.split();
    bool splittable = meshptrs.size() > 1;
    for (TriangleMesh* m : meshptrs) { delete m; }

    return splittable;
}

void ObjectList::part_settings_changed()
{
    m_part_settings_changed = true;
    wxGetApp().plater()->changed_object(get_selected_obj_idx());
    m_part_settings_changed = false;
}

void ObjectList::parts_changed(int obj_idx)
{
    wxGetApp().plater()->changed_object(obj_idx);
    m_parts_changed = false;
}

void ObjectList::part_selection_changed()
{
    int obj_idx = -1;
    m_config = nullptr;
    wxString og_name = wxEmptyString;

    bool update_and_show_manipulations = false;
    bool update_and_show_settings = false;

    if (multiple_selection()) {
        og_name = _(L("Group manipulation"));
        update_and_show_manipulations = true;
    }
    else
    {
        const auto item = GetSelection();
        if (item)
        {
            bool is_part = false;
            if (m_objects_model->GetParent(item) == wxDataViewItem(0)) {
                obj_idx = m_objects_model->GetIdByItem(item);
                og_name = _(L("Object manipulation"));
                m_config = &(*m_objects)[obj_idx]->config;
                update_and_show_manipulations = true;
            }
            else {
                auto parent = m_objects_model->GetParent(item);
                // Take ID of the parent object to "inform" perl-side which object have to be selected on the scene
                obj_idx = m_objects_model->GetIdByItem(parent);
                if (m_objects_model->GetItemType(item) == itSettings) {
                    if (m_objects_model->GetParent(parent) == wxDataViewItem(0)) {
                        og_name = _(L("Object Settings to modify"));
                        m_config = &(*m_objects)[obj_idx]->config;
                    }
                    else {
                        og_name = _(L("Part Settings to modify"));
                        is_part = true;
                        auto main_parent = m_objects_model->GetParent(parent);
                        obj_idx = m_objects_model->GetIdByItem(main_parent);
                        const auto volume_id = m_objects_model->GetVolumeIdByItem(parent);
                        m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
                    }
                    update_and_show_settings = true;
                }
                else if (m_objects_model->GetItemType(item) == itVolume) {
                    og_name = _(L("Part manipulation"));
                    is_part = true;
                    const auto volume_id = m_objects_model->GetVolumeIdByItem(item);
                    m_config = &(*m_objects)[obj_idx]->volumes[volume_id]->config;
                    update_and_show_manipulations = true;
                }
                else if (m_objects_model->GetItemType(item) == itInstance) {
                    og_name = _(L("Instance manipulation"));
                    update_and_show_manipulations = true;
                }
            }

            if (m_default_config) delete m_default_config;
            m_default_config = DynamicPrintConfig::new_from_defaults_keys(get_options(is_part));
        }
    }

    m_selected_object_id = obj_idx;

    if (update_and_show_manipulations) {
        wxGetApp().obj_manipul()->get_og()->set_name(" " + og_name + " ");
        wxGetApp().obj_manipul()->get_og()->set_value("object_name", m_objects_model->GetName(GetSelection()));
    }

    if (update_and_show_settings)
        wxGetApp().obj_settings()->get_og()->set_name(" " + og_name + " ");

    Sidebar& panel = wxGetApp().sidebar();
    panel.Freeze();

    wxGetApp().obj_manipul() ->UpdateAndShow(update_and_show_manipulations);
    wxGetApp().obj_settings()->UpdateAndShow(update_and_show_settings);
    wxGetApp().sidebar().show_info_sizer();

    panel.Layout();
    panel.Thaw();
}

void ObjectList::add_object_to_list(size_t obj_idx)
{
    auto model_object = (*m_objects)[obj_idx];
    wxString item_name = model_object->name;
    const auto item = m_objects_model->Add(item_name,
                      !model_object->config.has("extruder") ? 0 :
                      model_object->config.option<ConfigOptionInt>("extruder")->value);

    // Add error icon if detected auto-repaire
    auto stats = model_object->volumes[0]->mesh.stl.stats;
    int errors = stats.degenerate_facets + stats.edges_fixed + stats.facets_removed +
        stats.facets_added + stats.facets_reversed + stats.backwards_edges;
    if (errors > 0) {
        wxVariant variant;
        variant << PrusaDataViewBitmapText(item_name, m_bmp_manifold_warning);
        m_objects_model->SetValue(variant, item, 0);
    }

    // add volumes to the object
    if (model_object->volumes.size() > 1) {
        for (auto id = 0; id < model_object->volumes.size(); id++) {
            auto vol_item = m_objects_model->AddVolumeChild(item,
                model_object->volumes[id]->name,
                model_object->volumes[id]->type()/*ModelVolume::MODEL_PART*/,
                !model_object->volumes[id]->config.has("extruder") ? 0 :
                model_object->volumes[id]->config.option<ConfigOptionInt>("extruder")->value,
                false);
            auto opt_keys = model_object->volumes[id]->config.keys();
            if (!opt_keys.empty() && !(opt_keys.size() == 1 && opt_keys[0] == "extruder")) {
                select_item(m_objects_model->AddSettingsChild(vol_item));
                Collapse(vol_item);
            }
        }
        Expand(item);
    }

    // add instances to the object, if it has those
    if (model_object->instances.size()>1)
        increase_object_instances(obj_idx, model_object->instances.size());

    // add settings to the object, if it has those
    auto opt_keys = model_object->config.keys();
    if (!opt_keys.empty() && !(opt_keys.size() == 1 && opt_keys[0] == "extruder")) {
        select_item(m_objects_model->AddSettingsChild(item));
        Collapse(item);
    }

#ifndef __WXOSX__ 
    selection_changed();
#endif //__WXMSW__
}

void ObjectList::delete_object_from_list()
{
    auto item = GetSelection();
    if (!item) 
        return;
    if (m_objects_model->GetParent(item) == wxDataViewItem(0))
        select_item(m_objects_model->Delete(item));
    else
        select_item(m_objects_model->Delete(m_objects_model->GetParent(item)));
}

void ObjectList::delete_object_from_list(const size_t obj_idx)
{
    select_item(m_objects_model->Delete(m_objects_model->GetItemById(obj_idx)));
}

void ObjectList::delete_volume_from_list(const size_t obj_idx, const size_t vol_idx)
{
    select_item(m_objects_model->Delete(m_objects_model->GetItemByVolumeId(obj_idx, vol_idx)));
}

void ObjectList::delete_instance_from_list(const size_t obj_idx, const size_t inst_idx)
{
    select_item(m_objects_model->Delete(m_objects_model->GetItemByInstanceId(obj_idx, inst_idx)));
}

void ObjectList::delete_from_model_and_list(const ItemType type, const int obj_idx, const int sub_obj_idx)
{
    if ( !(type&(itObject|itVolume|itInstance)) )
        return;

    if (type&itObject) {
        del_object(obj_idx);
        delete_object_from_list(obj_idx);
    }
    else {
        del_subobject_from_object(obj_idx, sub_obj_idx, type);

        type == itVolume ? delete_volume_from_list(obj_idx, sub_obj_idx) :
            delete_instance_from_list(obj_idx, sub_obj_idx);
    }
}

void ObjectList::delete_from_model_and_list(const std::vector<ItemForDelete>& items_for_delete)
{
    if (items_for_delete.empty())
        return;

    for (std::vector<ItemForDelete>::const_reverse_iterator item = items_for_delete.rbegin(); item != items_for_delete.rend(); ++item)
    {
        if (!(item->type&(itObject | itVolume | itInstance)))
            continue;
        if (item->type&itObject) {
            del_object(item->obj_idx);
            m_objects_model->Delete(m_objects_model->GetItemById(item->obj_idx));
        }
        else {
            if (!del_subobject_from_object(item->obj_idx, item->sub_obj_idx, item->type))
                continue;
            if (item->type&itVolume)
            {
                m_objects_model->Delete(m_objects_model->GetItemByVolumeId(item->obj_idx, item->sub_obj_idx));
                wxGetApp().plater()->canvas3D()->ensure_on_bed(item->obj_idx);
            }
            else
                m_objects_model->Delete(m_objects_model->GetItemByInstanceId(item->obj_idx, item->sub_obj_idx));
        }
    }
    part_selection_changed();
}

void ObjectList::delete_all_objects_from_list()
{
    m_objects_model->DeleteAll();
    part_selection_changed();
}

void ObjectList::increase_object_instances(const size_t obj_idx, const size_t num)
{
    select_item(m_objects_model->AddInstanceChild(m_objects_model->GetItemById(obj_idx), num));
}

void ObjectList::decrease_object_instances(const size_t obj_idx, const size_t num)
{
    select_item(m_objects_model->DeleteLastInstance(m_objects_model->GetItemById(obj_idx), num));
}

void ObjectList::unselect_objects()
{
    if (!GetSelection())
        return;

    m_prevent_list_events = true;
    UnselectAll();
    part_selection_changed();
    m_prevent_list_events = false;
}

void ObjectList::select_current_object(int idx)
{
    m_prevent_list_events = true;
    UnselectAll();
    if (idx >= 0)
        Select(m_objects_model->GetItemById(idx));
    part_selection_changed();
    m_prevent_list_events = false;
}

void ObjectList::select_current_volume(int idx, int vol_idx)
{
    if (vol_idx < 0) {
        select_current_object(idx);
        return;
    }
    m_prevent_list_events = true;
    UnselectAll();
    if (idx >= 0)
        Select(m_objects_model->GetItemByVolumeId(idx, vol_idx));
    part_selection_changed();
    m_prevent_list_events = false;
}

void ObjectList::remove()
{
    if (GetSelectedItemsCount() == 0)
        return;

    wxDataViewItemArray sels;
    GetSelections(sels);

    for (auto& item : sels)
    {
        if (m_objects_model->GetParent(item) == wxDataViewItem(0))
            delete_from_model_and_list(itObject, m_objects_model->GetIdByItem(item), -1);
        else
            del_subobject_item(item);
    }
}

void ObjectList::init_objects()
{
    m_objects = wxGetApp().model_objects();
}

bool ObjectList::multiple_selection() const 
{
    return GetSelectedItemsCount() > 1;
}

void ObjectList::update_selections()
{
    auto& selection = wxGetApp().plater()->canvas3D()->get_selection();
    wxDataViewItemArray sels;

    // We doesn't update selection if SettingsItem for the current object/part is selected
    if (GetSelectedItemsCount() == 1 && m_objects_model->GetItemType(GetSelection()) == itSettings )
    {
        const auto item = GetSelection();
        if (selection.is_single_full_object() && 
            m_objects_model->GetIdByItem(m_objects_model->GetParent(item)) == selection.get_object_idx())
            return; 
        if (selection.is_single_volume() || selection.is_modifier()) {
            const auto gl_vol = selection.get_volume(*selection.get_volume_idxs().begin());
            if (m_objects_model->GetVolumeIdByItem(m_objects_model->GetParent(item)) == gl_vol->volume_idx())
                return;
        }
    }

    if (selection.is_single_full_object())
    {
        sels.Add(m_objects_model->GetItemById(selection.get_object_idx()));
    }
    else if (selection.is_single_volume() || selection.is_modifier() || 
             selection.is_multiple_volume() || selection.is_multiple_full_object()) {
        for (auto idx : selection.get_volume_idxs()) {
            const auto gl_vol = selection.get_volume(idx);
            if (selection.is_multiple_full_object())
                sels.Add(m_objects_model->GetItemById(gl_vol->object_idx()));
			else if (gl_vol->volume_idx() >= 0)
                // Only add GLVolumes with non-negative volume_ids. GLVolumes with negative volume ids
                // are not associated with ModelVolumes, but they are temporarily generated by the backend
                // (for example, SLA supports or SLA pad).
                sels.Add(m_objects_model->GetItemByVolumeId(gl_vol->object_idx(), gl_vol->volume_idx()));
        }
    }
    else if (selection.is_single_full_instance() || selection.is_multiple_full_instance()) {
        for (auto idx : selection.get_instance_idxs()) {            
            sels.Add(m_objects_model->GetItemByInstanceId(selection.get_object_idx(), idx));
        }
    }
    else if (selection.is_mixed())
    {
        auto& objects_content_list = selection.get_content();

        for (auto idx : selection.get_volume_idxs()) {
            const auto gl_vol = selection.get_volume(idx);
            const auto& glv_obj_idx = gl_vol->object_idx();
            const auto& glv_ins_idx = gl_vol->instance_idx();

            bool is_selected = false;

            for (auto obj_ins : objects_content_list) {
                if (obj_ins.first == glv_obj_idx) {
                    if (obj_ins.second.find(glv_ins_idx) != obj_ins.second.end()) {
                        if (glv_ins_idx == 0 && (*m_objects)[glv_obj_idx]->instances.size() == 1)
                            sels.Add(m_objects_model->GetItemById(glv_obj_idx));
                        else
                            sels.Add(m_objects_model->GetItemByInstanceId(glv_obj_idx, glv_ins_idx));

                        is_selected = true;
                        break;
                    }
                }
            }

            if (is_selected)
                continue;

            const auto& glv_vol_idx = gl_vol->volume_idx();
            if (glv_vol_idx == 0 && (*m_objects)[glv_obj_idx]->volumes.size() == 1)
                sels.Add(m_objects_model->GetItemById(glv_obj_idx));
            else
                sels.Add(m_objects_model->GetItemByVolumeId(glv_obj_idx, glv_vol_idx));
        }
    }
    
    select_items(sels);

    if (GetSelection()) {
        const int sel_item_row = m_objects_model->GetRowByItem(GetSelection());
        ScrollLines(sel_item_row - m_selected_row);
        m_selected_row = sel_item_row;
    }
}

void ObjectList::update_selections_on_canvas()
{
    auto& selection = wxGetApp().plater()->canvas3D()->get_selection();

    const int sel_cnt = GetSelectedItemsCount();
    if (sel_cnt == 0) {
        selection.clear();
        wxGetApp().plater()->canvas3D()->update_gizmos_on_off_state();
        return;
    }

    auto add_to_selection = [this](const wxDataViewItem& item, GLCanvas3D::Selection& selection, bool as_single_selection)
    {        
        if (m_objects_model->GetParent(item) == wxDataViewItem(0)) {
            selection.add_object(m_objects_model->GetIdByItem(item), as_single_selection);
            return;
        }

        if (m_objects_model->GetItemType(item) == itVolume) {
            const int obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetParent(item));
            const int vol_idx = m_objects_model->GetVolumeIdByItem(item);
            selection.add_volume(obj_idx, vol_idx, 0, as_single_selection);
        }
        else if (m_objects_model->GetItemType(item) == itInstance) {
            const int obj_idx = m_objects_model->GetIdByItem(m_objects_model->GetTopParent(item));
            const int inst_idx = m_objects_model->GetInstanceIdByItem(item);
            selection.add_instance(obj_idx, inst_idx, as_single_selection);
        }
    };

    if (sel_cnt == 1) {
        wxDataViewItem item = GetSelection();
        if (m_objects_model->GetItemType(item) & (itSettings|itInstanceRoot))
            add_to_selection(m_objects_model->GetParent(item), selection, true);
        else
            add_to_selection(item, selection, true);
            
        wxGetApp().plater()->canvas3D()->update_gizmos_on_off_state();
        return;
    }
    
    wxDataViewItemArray sels;
    GetSelections(sels);

    selection.clear();
    for (auto item: sels)
        add_to_selection(item, selection, false);

    wxGetApp().plater()->canvas3D()->update_gizmos_on_off_state();
}

void ObjectList::select_item(const wxDataViewItem& item)
{
    m_prevent_list_events = true;

    UnselectAll();
    Select(item);
    part_selection_changed();

    m_prevent_list_events = false;
}

void ObjectList::select_items(const wxDataViewItemArray& sels)
{
    m_prevent_list_events = true;

    UnselectAll();
    SetSelections(sels);
    part_selection_changed();

    m_prevent_list_events = false;
}

void ObjectList::select_all()
{
    SelectAll();
    selection_changed();
}

void ObjectList::select_item_all_children()
{
    wxDataViewItemArray sels;

    // There is no selection before OR some object is selected   =>  select all objects
    if (!GetSelection() || m_objects_model->GetItemType(GetSelection()) == itObject) {
        for (int i = 0; i < m_objects->size(); i++)
            sels.Add(m_objects_model->GetItemById(i));
    }
    else {
        const auto item = GetSelection();
        // Some volume(instance) is selected    =>  select all volumes(instances) inside the current object
        if (m_objects_model->GetItemType(item) & (itVolume | itInstance)) {
            m_objects_model->GetChildren(m_objects_model->GetParent(item), sels);
        }
    }

    SetSelections(sels);
    selection_changed();
}

void ObjectList::fix_multiselection_conflicts()
{
    if (GetSelectedItemsCount() <= 1)
        return;

    m_prevent_list_events = true;

    wxDataViewItemArray sels;
    GetSelections(sels);

    for (auto item : sels) {
        if (m_objects_model->GetItemType(item) & (itSettings|itInstanceRoot))
            Unselect(item);
        else if (m_objects_model->GetParent(item) != wxDataViewItem(0))
            Unselect(m_objects_model->GetParent(item));
    }

    m_prevent_list_events = false;
}

ModelVolume* ObjectList::get_selected_model_volume()
{
    auto item = GetSelection();
    if (!item || m_objects_model->GetItemType(item) != itVolume)
        return nullptr;

    const auto vol_idx = m_objects_model->GetVolumeIdByItem(item);
    const auto obj_idx = get_selected_obj_idx();
    if (vol_idx < 0 || obj_idx < 0)
        return nullptr;

    return (*m_objects)[obj_idx]->volumes[vol_idx];
}

void ObjectList::change_part_type()
{
    ModelVolume* volume = get_selected_model_volume();
    if (!volume)
        return;

    const auto type = volume->type();
    if (type == ModelVolume::MODEL_PART)
    {
        const int obj_idx = get_selected_obj_idx();
        if (obj_idx < 0) return;

        int model_part_cnt = 0;
        for (auto vol : (*m_objects)[obj_idx]->volumes) {
            if (vol->type() == ModelVolume::MODEL_PART)
                ++model_part_cnt;
        }

        if (model_part_cnt == 1) {
            Slic3r::GUI::show_error(nullptr, _(L("You can't change a type of the last solid part of the object.")));
            return;
        }
    }

    const wxString names[] = { "Part", "Modifier", "Support Enforcer", "Support Blocker" };
    
    auto new_type = wxGetSingleChoiceIndex("Type: ", _(L("Select type of part")), wxArrayString(4, names), type);

    if (new_type == type || new_type < 0)
        return;

    const auto item = GetSelection();
    volume->set_type(static_cast<ModelVolume::Type>(new_type));
    m_objects_model->SetVolumeType(item, new_type);

    m_parts_changed = true;
    parts_changed(get_selected_obj_idx());

    // Update settings showing, if we have it
    //(we show additional settings for Part and Modifier and hide it for Support Blocker/Enforcer)
    const auto settings_item = m_objects_model->GetSettingsItem(item);
    if (settings_item && 
        (new_type == ModelVolume::SUPPORT_ENFORCER || new_type == ModelVolume::SUPPORT_BLOCKER)) {
        m_objects_model->Delete(settings_item);
    }
    else if (!settings_item && 
              (new_type == ModelVolume::MODEL_PART || new_type == ModelVolume::PARAMETER_MODIFIER)) {
        select_item(m_objects_model->AddSettingsChild(item));
    }
}

void ObjectList::last_volume_is_deleted(const int obj_idx)
{

    if (obj_idx < 0 || m_objects->empty() ||
        obj_idx <= m_objects->size() ||
        (*m_objects)[obj_idx]->volumes.empty())
        return;
    auto volume = (*m_objects)[obj_idx]->volumes[0];

    // clear volume's config values
    volume->config.clear();

    // set a default extruder value, since user can't add it manually
    volume->config.set_key_value("extruder", new ConfigOptionInt(0));
}

bool ObjectList::has_multi_part_objects()
{
    if (!m_objects_model->IsEmpty()) {
        wxDataViewItemArray items;
        m_objects_model->GetChildren(wxDataViewItem(0), items);

        for (auto& item : items)
            if (m_objects_model->GetItemByType(item, itVolume))
                return true;
    }
    return false;
}

void ObjectList::update_settings_items()
{
    wxDataViewItemArray items;
    m_objects_model->GetChildren(wxDataViewItem(0), items);

    for (auto& item : items) {        
        const wxDataViewItem& settings_item = m_objects_model->GetSettingsItem(item);
        select_item(settings_item ? settings_item : m_objects_model->AddSettingsChild(item));
    }
    UnselectAll();
}

void ObjectList::ItemValueChanged(wxDataViewEvent &event)
{
    if (event.GetColumn() == 0)
        update_name_in_model(event.GetItem());
    else if (event.GetColumn() == 1)
        update_extruder_in_config(event.GetItem());
}

void ObjectList::OnEditingDone(wxDataViewEvent &event)
{
    if (event.GetColumn() != 0)
        return;

    const auto renderer = dynamic_cast<PrusaBitmapTextRenderer*>(GetColumn(0)->GetRenderer());

    if (renderer->WasCanceled())
        show_error(this, _(L("The supplied name is not valid;")) + "\n" +
                         _(L("the following characters are not allowed:")) + " <>:/\\|?*\"");
}

} //namespace GUI
} //namespace Slic3r 