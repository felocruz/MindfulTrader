# -*- coding: utf-8 -*-
"""
Created on Thu Jan 14 08:26:55 2021

@author: Rafael Cruz, Ph.D.

Configuration file that stores trading parameters
"""
import logging
from typing import Dict
from dash import html, dcc
import dash_bootstrap_components as dbc
from dash_util import APP_CARD_BACKGROUND_COLOR, APP_INPUT_BACKGROUND_COLOR, APP_LIGHT_COLOR, card_header

GOOGLE_API_KEY = 'AIzaSyDoycDPSJeL-8Z205LxSC8hLqEbtRtlMSY'


# --- Logging Configuration (Consolidated and Improved) ---
# It's best to configure logging once and early.
# Get a logger specific to this configuration module.
logger = logging.getLogger(__name__)
# Basic configuration, you might want a more sophisticated setup for larger apps.
logging.basicConfig(
    level=logging.INFO,  # Default logging level
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)

# =================== Sierra Chart ==================================

ZMQ_ENDPOINT = "tcp://127.0.0.1:5555"
ZMQ_TRADE_ENDPOINT = "tcp://127.0.0.1:5556"
ZMQ_INITIAL_REQ_ENDPOINT = "tcp://127.0.0.1:5557"

SIERRA_CHART_IP_ADDRESS: str = "127.0.0.1"
SIERRA_CHART_PORT: int = 4445

# The subscription topics for the SUB socket.
# INSTITUTIONAL: Message types must match exactly with sender (zmq_client.py)
# These are used for content-based routing in websocket_broadcaster._process_queue_batch()
# DEPRECATED (Feb 10, 2026): BAR_CLOSE_UPDATE and INITIALIZE_SEQUENCE no longer sent from C++
INDICATOR_UPDATE_TOPIC = "indicator_update"  # Refactored from "indicator" (Feb 10, 2026)
POSITION_UPDATE_TOPIC = "position"

# High grade thresholds
A_GRADE: int = 30
A_GRADE_PCT: float = 0.30

# Trade journal analysis
CHARTS_DIR = "C:/Trading/charts"
PLACEHOLDER_CHART = 'placeholder.png'

ES_SWING_SETTINGS: Dict[str, str] = {
    'current_symbol': 'ES',
    'ib_symbol': 'ES-202112-GLOBEX',
    'short_graph_file': 'C:/SierraChart2/Data/ES_GraphData.csv',
    'long_graph_file': 'C:/SierraChart2/Data/ES_GraphData_LTF.csv'
}

ES_SWING_SETTINGS_DAILY: Dict[str, str] = {
    'current_symbol': 'ES',
    'ib_symbol': 'ES-202112-GLOBEX',
    'short_graph_file': 'C:/SierraChart2/Data/ES_Daily_GraphData.csv',
    'long_graph_file': 'C:/SierraChart2/Data/ES_Weekly_GraphData.csv'
}

MES_SWING_SETTINGS_DAILY: Dict[str, str] = {
    'current_symbol': 'MES',
    'ib_symbol': 'MES-202409-CME-USD',
    'short_graph_file': 'C:/SierraChart2/Data/MES_Daily_GraphData.csv',
    'long_graph_file': 'C:/SierraChart2/Data/MES_Weekly_GraphData.csv'
}

FXAIX_SETTINGS: Dict[str, str] = {
    'current_symbol': 'FXAIX',
    'ib_symbol': 'ES-202112-GLOBEX',
    'short_graph_file': 'C:/SierraChart2/Data/FXAIX_GraphData.csv',
    'long_graph_file': 'C:/SierraChart2/Data/FXAIX_GraphData_LTF.csv'
}

# settings = ES_SWING_SETTINGS_DAILY
settings = MES_SWING_SETTINGS_DAILY

# Debug Flags (Consolidated into a single dictionary for easier management and less global variables)
DEBUG_FLAGS: Dict[str, bool] = {
    'APP': True,
    'DATABASE_MANAGER': False,
    'INDICATOR_MANAGER': False,
    'POSITION_MANAGER': False,
    'ACTION_PLAN': False,
    'MARKETS': False,
    'SPREADSHEET': False,
    'TRADE_JOURNAL': False,
    'TRADE_ANALYTICS': False,
    'STRATEGY': False,
    'TACTICS': False,
    'TECHNICAL_ANALYSIS': False,
    'MARKET_MINDER': False,
    'FIELD': False,
    'IB': False,
    'NEW_FEATURE' : False,
    'ZMQ_CLIENT': False,
    'ZMQ_SERVER': False
}


# You can define a helper to easily check debug flags
def is_debug_enabled(feature: str) -> bool:
    """Checks if a specific debug feature is enabled."""
    return DEBUG_FLAGS.get(feature, False)


class Config:
    """ Application settings """

    def __init__(self):
        self.settings: dict = {
            'current_symbol': 'MES',
            'ib_symbol': 'MES-202309-CME-USD',
        }

    def register_callbacks(self, app):
        """ Register component callbacks """

    @staticmethod
    def render():
        """ Updates the status row with the new values. Does not update if there are no changes """
        return dbc.Card(
            [
                card_header("Configuration Panel"),
                dbc.CardBody(
                    [
                        dbc.Button("Submit", id='config-submit-button', n_clicks=0),
                        dbc.Row([
                            dbc.Col([dbc.Label(children="Current Symbol", color="orange"),
                                     dbc.Input(id='config_current_symbol', value='FXAIX',
                                               type='text',
                                               style={'backgroundColor': APP_INPUT_BACKGROUND_COLOR,
                                                      'color': APP_LIGHT_COLOR})]),
                            dbc.Col([dbc.Label(children="IB Symbol", color="orange"),
                                     dbc.Input(id='config_ib_symbol', value='ES-202112-GLOBEX',
                                               type='text',
                                               style={'backgroundColor': APP_INPUT_BACKGROUND_COLOR,
                                                      'color': APP_LIGHT_COLOR})]),
                            dbc.Col(dcc.Checklist(
                                id="play_sound",  # The ID for your checkbox
                                options=[
                                    {'label': 'Play Sound', 'value': 'SOUND_ON'}  # Define the single option
                                ],
                                value=[],  # Initial state: empty list means unchecked
                                # The 'value' property of dcc.Checklist is a list of selected values.
                                # So, [] for unchecked, and ['SOUND_ON'] for checked.
                                style={'padding': '10px', 'border': '1px solid #ddd', 'borderRadius': '5px'}
                                # Basic styling
                            ), )
                        ]),
                        html.Div(id='config-output')
                    ])
            ], color=APP_CARD_BACKGROUND_COLOR)

    @property
    def current_symbol(self):
        """ Returns the symbol being traded in Sierra Chart """
        return self.settings['current_symbol']

    @property
    def ib_symbol(self):
        """ Returns the symbol being traded in IB """
        return self.settings['ib_symbol']


config_manager = Config()
